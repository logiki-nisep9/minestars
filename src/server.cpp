/*
Minetest
Copyright (C) 2010-2013 celeron55, Perttu Ahola <celeron55@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation; either version 3.0 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "server.h"
#include <cstdint>
#include <iostream>
#include <queue>
#include <random>
#include <algorithm>
#include "debug.h"
#include "network/mt_connection.h"
#include "network/networkpacket.h"
#include "network/networkprotocol.h"
#include "network/serveropcodes.h"
#include "ban.h"
#include "environment.h"
#include "map.h"
#include "threading/mutex_auto_lock.h"
#include "constants.h"
#include "voxel.h"
#include "config.h"
#include "version.h"
#include "filesys.h"
#include "mapblock.h"
#include "server/serveractiveobject.h"
#include "settings.h"
#include "profiler.h"
#include "log.h"
#include "scripting_server.h"
#include "nodedef.h"
#include "itemdef.h"
#include "craftdef.h"
#include "emerge.h"
#include "mapgen/mapgen.h"
#include "mapgen/mg_biome.h"
#include "content_mapnode.h"
#include "content_nodemeta.h"
#include "content/mods.h"
#include "modchannels.h"
#include "serverlist.h"
#include "util/string.h"
#if USE_SQLITE
#include "rollback.h"
#endif
#include "util/serialize.h"
#include "util/thread.h"
#include "defaultsettings.h"
#include "server/mods.h"
#include "util/base64.h"
#include "util/sha1.h"
#include "util/hex.h"
#include "database/database.h"
#include "chatmessage.h"
#include "chat_interface.h"
#include "remoteplayer.h"
#include "server/player_sao.h"
#include "server/serverinventorymgr.h"
#include "translation.h"
#include "ServerNetworkEngine.h"
#include "slave_helpers.h"
#include "slave_proxy_net/objects_id_logic.h"

class ClientNotFoundException : public BaseException
{
public:
	ClientNotFoundException(const char *s):
		BaseException(s)
	{}
};

class ServerThread : public Thread
{
public:

	ServerThread(Server *server):
		Thread("Serv{AO,Net}"),
		m_server(server)
	{}

	void *run();

private:
	Server *m_server;
};

void *ServerThread::run()
{
	BEGIN_DEBUG_EXCEPTION_HANDLER

	/*
	 * The real business of the server happens on the ServerThread.
	 * How this works:
	 * AsyncRunStep() runs an actual server step as soon as enough time has
	 * passed (dedicated_server_loop keeps track of that).
	 * Receive() blocks at least(!) 30ms waiting for a packet (so this loop
	 * doesn't busy wait) and will process any remaining packets.
	 */

	m_server->AsyncRunStep(true);

	while (!stopRequested()) {
		try {
			m_server->AsyncRunStep();

			if (m_server->ServersNetworkObject->AreSlave)
				m_server->Receive();
			else
				std::this_thread::sleep_for(std::chrono::milliseconds(20)); //sleep

		} catch (con::PeerNotFoundException &e) {
			infostream<<"Server: PeerNotFoundException"<<std::endl;
		} catch (ClientNotFoundException &e) {
		} catch (con::ConnectionBindFailed &e) {
			m_server->setAsyncFatalError(e.what());
		} catch (LuaError &e) {
			m_server->setAsyncFatalError(
					"ServerThread::run Lua: " + std::string(e.what()));
		}
	}

	END_DEBUG_EXCEPTION_HANDLER

	return nullptr;
}

class ServSectorEnv: public Thread {
public:
	ServSectorEnv(Server *s): Thread("Serv{Env}"), m_server(s) {};
	void *run() {
		while (!stopRequested()) {
			//errorstream << "a" << std::endl;
			m_server->step(0.02); //Script on other thread....
			m_server->m_env->stepScript(0.02);
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
	}
private:
	Server *m_server = nullptr;
};

class SentNetworkThread: public Thread {
public:
	SentNetworkThread(Server *Serv): Thread("Network[Sent]"), m_server(Serv) {}
	void *run() {
		while (!stopRequested()) {
			m_server->PacketsDequeMTX.lock();
			if (!m_server->PacketsDeque.empty()) {
				NetworkPacket pkt = m_server->PacketsDeque.front();
				//actionstream << "Got packet to stream: " << pkt.getPeerId() << " | cmd=" << pkt.getCommand() << std::endl;
				if (!pkt.didSetReliableOpt())
					m_server->Send(&pkt, pkt.reliableOption());
				else
					m_server->Send(&pkt);
				m_server->PacketsDeque.pop_front();
			}
			m_server->PacketsDequeMTX.unlock();
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return nullptr;
	}
private:
	std::mutex mtx;
	Server *m_server = nullptr;
};

//Network thread [does check the network] [Only runs in proxy]
class NetworkThread : public Thread {
public:

	NetworkThread(Server *server):
		Thread("Network[Main]"),
		m_server(server)
	{}
	void *run();
	NetworkPacket getPacket() {
		_mutex.lock();
		NetworkPacket pkt;
		if (!packets.empty()) {
			pkt = (packets.size() > 0) ? packets.front() : NetworkPacket();
			packets.pop_front();
		}
		_mutex.unlock();
		return pkt;
	}
	std::atomic<int> m_parent_skt;
private:
	Server *m_server = nullptr;

	//NOTE: ONLY USED IF WORKING ON A SOCKET
	std::deque<NetworkPacket> packets;
	std::mutex _mutex;
};



//NetworkCompressor [This will send all queued packets into 1 packet for no packet loss]



v3f ServerSoundParams::getPos(ServerEnvironment *env, bool *pos_exists) const
{
	if(pos_exists) *pos_exists = false;
	switch(type){
	case SSP_LOCAL:
		return v3f(0,0,0);
	case SSP_POSITIONAL:
		if(pos_exists) *pos_exists = true;
		return pos;
	case SSP_OBJECT: {
		if(object == 0)
			return v3f(0,0,0);
		ServerActiveObject *sao = env->getActiveObject(object);
		if(!sao)
			return v3f(0,0,0);
		if(pos_exists) *pos_exists = true;
		return sao->getBasePosition(); }
	}
	return v3f(0,0,0);
}

void Server::ShutdownState::reset()
{
	m_timer = 0.0f;
	message.clear();
	should_reconnect = false;
	is_requested = false;
}

void Server::ShutdownState::trigger(float delay, const std::string &msg, bool reconnect)
{
	m_timer = delay;
	message = msg;
	should_reconnect = reconnect;
}

void Server::ShutdownState::tick(float dtime, Server *server)
{
	if (m_timer <= 0.0f)
		return;

	// Timed shutdown
	static const float shutdown_msg_times[] =
	{
		1, 2, 3, 4, 5, 10, 20, 40, 60, 120, 180, 300, 600, 1200, 1800, 3600
	};

	// Automated messages
	if (m_timer < shutdown_msg_times[ARRLEN(shutdown_msg_times) - 1]) {
		for (float t : shutdown_msg_times) {
			// If shutdown timer matches an automessage, shot it
			if (m_timer > t && m_timer - dtime < t) {
				std::wstring periodicMsg = getShutdownTimerMessage();

				infostream << wide_to_utf8(periodicMsg).c_str() << std::endl;
				if (!server->ServersNetworkObject->AreSlave)
					server->SendChatMessage(PEER_ID_INEXISTENT, periodicMsg);
				else
					server->SendChatMessage(maskedu16(), periodicMsg);
				break;
			}
		}
	}

	m_timer -= dtime;
	if (m_timer < 0.0f) {
		m_timer = 0.0f;
		is_requested = true;
	}
}

std::wstring Server::ShutdownState::getShutdownTimerMessage() const
{
	std::wstringstream ws;
	ws << L"*** Server shutting down in "
		<< duration_to_string(myround(m_timer)).c_str() << ".";
	return ws.str();
}

/*
	Server
*/

Server::Server(
		const std::string &path_world,
		const SubgameSpec &gamespec,
		bool simple_singleplayer_mode,
		Address bind_addr,
		bool dedicated,
		ChatInterface *iface,
		std::string *on_shutdown_errmsg
	):
	m_bind_addr(bind_addr),
	m_path_world(path_world),
	m_gamespec(gamespec),
	m_simple_singleplayer_mode(simple_singleplayer_mode),
	m_dedicated(dedicated),
	m_async_fatal_error(""),
	m_con(std::make_shared<con::Connection>(PROTOCOL_ID, 512, CONNECTION_TIMEOUT, m_bind_addr.isIPv6(), this)),
	MAINSERVEURE(std::make_shared<con::Connection>(PROTOCOL_ID, 512, CONNECTION_TIMEOUT, m_bind_addr.isIPv6(), this)),
	m_itemdef(createItemDefManager()),
	m_nodedef(createNodeDefManager()),
	m_craftdef(createCraftDefManager()),
	m_thread(new ServerThread(this)),
	m_netthr(new NetworkThread(this)),
	m_clients(m_con),
	m_admin_chat(iface),
	m_on_shutdown_errmsg(on_shutdown_errmsg),
	m_async_globals_data(""),
	m_modchannel_mgr(new ModChannelMgr())
{
	if (m_path_world.empty())
		throw ServerError("Supplied empty world path");

	if (!gamespec.isValid())
		throw ServerError("Supplied invalid gamespec");

#if USE_PROMETHEUS
	m_metrics_backend = std::unique_ptr<MetricsBackend>(createPrometheusMetricsBackend());
#else
	m_metrics_backend = std::unique_ptr<MetricsBackend>(new MetricsBackend());
#endif

	m_uptime_counter = m_metrics_backend->addCounter("minetest_core_server_uptime", "Server uptime (in seconds)");
	m_player_gauge = m_metrics_backend->addGauge("minetest_core_player_number", "Number of connected players");

	m_timeofday_gauge = m_metrics_backend->addGauge(
			"minetest_core_timeofday",
			"Time of day value");

	m_lag_gauge = m_metrics_backend->addGauge(
			"minetest_core_latency",
			"Latency value (in seconds)");

	m_aom_buffer_counter = m_metrics_backend->addCounter(
			"minetest_core_aom_generated_count",
			"Number of active object messages generated");

	m_packet_recv_counter = m_metrics_backend->addCounter(
			"minetest_core_server_packet_recv",
			"Processable packets received");

	m_packet_recv_processed_counter = m_metrics_backend->addCounter(
			"minetest_core_server_packet_recv_processed",
			"Valid received packets processed");

	m_lag_gauge->set(g_settings->getFloat("dedicated_server_step"));
	
	ServersNetworkObject = new ServerNetworkEngine(m_env, this, m_path_world);
	NetCR = new NetworkCompressor(this);


	if (SocketConn) {
		warningstream << "Deleting networks of server-slave as it will don't be used" << std::endl;
		m_con.reset();
		MAINSERVEURE.reset();
	}
}

Server::~Server()
{

	// Send shutdown message
	if (!ServersNetworkObject->AreSlave)
		SendChatMessage(PEER_ID_INEXISTENT, ChatMessage(CHATMESSAGE_TYPE_ANNOUNCE, L"*** Server shutting down"));
	else
		SendChatMessage(maskedu16(), ChatMessage(CHATMESSAGE_TYPE_ANNOUNCE, L"*** Server shutting down"));

	if (m_env) {
		MutexAutoLock envlock(m_env_mutex);

		infostream << "Server: Saving players" << std::endl;
		m_env->saveLoadedPlayers();

		infostream << "Server: Kicking players" << std::endl;
		std::string kick_msg;
		bool reconnect = false;
		if (isShutdownRequested()) {
			reconnect = m_shutdown_state.should_reconnect;
			kick_msg = m_shutdown_state.message;
		}
		if (kick_msg.empty()) {
			kick_msg = g_settings->get("kick_msg_shutdown");
		}
		m_env->saveLoadedPlayers(true);
		m_env->kickAllPlayers(SERVER_ACCESSDENIED_SHUTDOWN,
			kick_msg, reconnect);
	}

	actionstream << "Server: Shutting down" << std::endl;

	// Do this before stopping the server in case mapgen callbacks need to access
	// server-controlled resources (like ModStorages). Also do them before
	// shutdown callbacks since they may modify state that is finalized in a
	// callback.
	if (m_emerge)
		m_emerge->stopThreads();

	if (m_env) {
		MutexAutoLock envlock(m_env_mutex);

		// Execute script shutdown hooks
		infostream << "Executing shutdown hooks" << std::endl;
		try {
			m_script->on_shutdown();
		} catch (ModError &e) {
			errorstream << "ModError: " << e.what() << std::endl;
			if (m_on_shutdown_errmsg) {
				if (m_on_shutdown_errmsg->empty()) {
					*m_on_shutdown_errmsg = std::string("ModError: ") + e.what();
				} else {
					*m_on_shutdown_errmsg += std::string("\nModError: ") + e.what();
				}
			}
		}

		infostream << "Server: Saving environment metadata" << std::endl;
		m_env->saveMeta();
	}

	// Stop threads
	if (m_thread) {
		stop();
		delete m_thread;
	}
	
	if (m_netthr) {
		m_netthr->stop();
		//delete m_netthr;
	}
	
	NetCR->stop();
	delete NetCR;

	// Delete things in the reverse order of creation
	delete m_emerge;
	delete m_env;
#if USE_SQLITE
	delete m_rollback;
#endif
	delete m_banmanager;
	delete m_itemdef;
	delete m_nodedef;
	delete m_craftdef;

	// Deinitialize scripting
	infostream << "Server: Deinitializing scripting" << std::endl;
	delete m_script;
	//delete m_startup_server_map; // if available
	delete m_game_settings;

	while (!m_unsent_map_edit_queue.empty()) {
		delete m_unsent_map_edit_queue.front();
		m_unsent_map_edit_queue.pop();
	}
}

void Server::init()
{
	infostream << "Server created for gameid \"" << m_gamespec.id << "\"";
	if (m_simple_singleplayer_mode)
		infostream << " in simple singleplayer mode" << std::endl;
	else
		infostream << std::endl;
	infostream << "- world:  " << m_path_world << std::endl;
	infostream << "- game:   " << m_gamespec.path << std::endl;

	m_game_settings = Settings::createLayer(SL_GAME);

	// Create world if it doesn't exist
	try {
		loadGameConfAndInitWorld(m_path_world,
				fs::GetFilenameFromPath(m_path_world.c_str()),
				m_gamespec, false);
	} catch (const BaseException &e) {
		throw ServerError(std::string("Failed to initialize world: ") + e.what());
	}

	// Create emerge manager
	m_emerge = new EmergeManager(this);

	// Create ban manager
	std::string ban_path = m_path_world + DIR_DELIM "ipban.txt";
	m_banmanager = new BanManager(ban_path);

	m_modmgr = std::unique_ptr<ServerModManager>(new ServerModManager(m_path_world));
	std::vector<ModSpec> unsatisfied_mods = m_modmgr->getUnsatisfiedMods();
	// complain about mods with unsatisfied dependencies
	if (!m_modmgr->isConsistent()) {
		m_modmgr->printUnsatisfiedModsError();
	}

	//lock environment
	MutexAutoLock envlock(m_env_mutex);

	// Create the Map (loads map_meta.txt, overriding configured mapgen params)
	ServerMap *servermap = new ServerMap(m_path_world, this, m_emerge, m_metrics_backend.get());
	m_startup_server_map = servermap;

	// Initialize scripting
	infostream << "Server: Initializing Lua" << std::endl;

	m_script = new ServerScripting(this);

	// Must be created before mod loading because we have some inventory creation
	m_inventory_mgr = std::unique_ptr<ServerInventoryManager>(new ServerInventoryManager());

	m_script->loadMod(getBuiltinLuaPath() + DIR_DELIM "init.lua", BUILTIN_MOD_NAME);

	m_modmgr->loadMods(m_script);

	// m_compat_player_models is used to prevent constant re-parsing of the
	// setting
	std::string player_models = g_settings->get("compat_player_model");
	player_models.erase(std::remove_if(player_models.begin(),
			player_models.end(), static_cast<int(*)(int)>(&std::isspace)),
			player_models.end());
	if (player_models.empty() || isSingleplayer())
		FATAL_ERROR_IF(!m_compat_player_models.empty(), "Compat player models list not empty");
	else
		m_compat_player_models = str_split(player_models, ',');

	m_script->saveGlobals();

	// Read Textures and calculate sha1 sums
	fillMediaCache();

	// Apply item aliases in the node definition manager
	if (!ServersNetworkObject->AreSlave)
		m_nodedef->updateAliases(m_itemdef);

	// Apply texture overrides from texturepack/override.txt
	std::vector<std::string> paths;
	fs::GetRecursiveDirs(paths, g_settings->get("texture_path"));
	fs::GetRecursiveDirs(paths, m_gamespec.path + DIR_DELIM + "textures");
	for (const std::string &path : paths) {
		TextureOverrideSource override_source(path + DIR_DELIM + "override.txt");
		m_nodedef->applyTextureOverrides(override_source.getNodeTileOverrides());
		m_itemdef->applyTextureOverrides(override_source.getItemTextureOverrides());
	}

	if (!ServersNetworkObject->AreSlave) {
		m_nodedef->setNodeRegistrationStatus(true);
		// Perform pending node name resolutions
		m_nodedef->runNodeResolveCallbacks();
		// unmap node names in cross-references
		m_nodedef->resolveCrossrefs();
		// init the recipe hashes to speed up crafting
		m_craftdef->initHashes(this);
		//m_emerge->initMapgens(m_startup_server_map->getMapgenParams());
	}

	// Initialize Environment
	//m_startup_server_map = nullptr; // Ownership moved to ServerEnvironment
	m_env = new ServerEnvironment(servermap, m_script, this, m_path_world);

	m_inventory_mgr->setEnv(m_env);
	m_clients.setEnv(m_env);

	if (!servermap->settings_mgr.makeMapgenParams())
		FATAL_ERROR("Couldn't create any mapgen type");

	// Initialize mapgens
	if (!ServersNetworkObject->AreSlave)
		m_emerge->initMapgens(servermap->getMapgenParams());

#if USE_SQLITE
	if (g_settings->getBool("enable_rollback_recording")) {
		// Create rollback manager
		m_rollback = new RollbackManager(m_path_world, this);
	}
#endif

	// Give environment reference to scripting api
	m_script->initializeEnvironment(m_env);

	// Do this after regular script init is done
	m_script->initAsync();

	// Register us to receive map edit events
	servermap->addEventReceiver(this);

	try {
		m_env->loadMeta();
	} catch (SerializationError &e) {
		warningstream << "Environment metadata is corrupted: " << e.what() << std::endl;
		warningstream << "Loading the default instead" << std::endl;
		m_env->loadDefaultMeta();
	}

	// Those settings can be overwritten in world.mt, they are
	// intended to be cached after environment loading.
	m_liquid_transform_every = g_settings->getFloat("liquid_update");
	m_max_chatmessage_length = g_settings->getU16("chat_message_max_size");
	m_csm_restriction_flags = g_settings->getU64("csm_restriction_flags");
	m_csm_restriction_noderange = g_settings->getU32("csm_restriction_noderange");

	ServersNetworkObject->SetEnv(m_env);
	if (ServersNetworkObject->AreSlave && !SocketConn)
		NetCR->start();

	if (!ServersNetworkObject->AreSlave) {
		SentNetworkThread *m_SNT = new SentNetworkThread(this);
		SNT = m_SNT;
		m_SNT->start();
	}
}

void Server::start()
{
	init();

	infostream << "Starting server on " << m_bind_addr.serializeString() << "..." << std::endl;

	// Stop thread if already running
	m_thread->stop();
	m_netthr->stop();

	m_thread->start();

	std::string flags = "";

	// Initialize connection
	if (!SocketConn && ServersNetworkObject->AreSlave) {
		m_con->SetTimeoutMs(30);
		m_con->Serve(m_bind_addr);
		flags += " SlaveServer::ClassicConnection ";
	} else if (ServersNetworkObject->AreSlave && SocketConn) {
		m_netthr->start();
		flags += " SlaveServer::SocketConn[Fast] ";
	} else if (!ServersNetworkObject->AreSlave) {
		m_con->SetTimeoutMs(30);
		m_con->Serve(m_bind_addr);
		m_netthr->start();
		flags += " MainServer ";
	}

	if (ServersNetworkObject->AreSlave)
		sendDefinitions();

	warningstream << "Server with flags: " << flags << std::endl;

	//Multithreaded essential
	//ServSectorScript *_SSS = new ServSectorScript(this);
	ServSectorEnv *_SSE = new ServSectorEnv(this);
	//SSS = _SSS;
	SSE = _SSE;
	//SSS->start();
	SSE->start();

	actionstream << "World at [" << m_path_world << "]" << std::endl;
	actionstream << "Server for gameid=\"" << m_gamespec.id
			<< "\" listening on " << m_bind_addr.serializeString() << ":"
			<< m_bind_addr.getPort() << "." << std::endl;
	
	//Start proxy
	ServersNetworkObject->OnInitServer();
}

void Server::stop()
{
	infostream<<"Server: Stopping and waiting threads"<<std::endl;

	// Stop threads (set run=false first so both start stopping)
	m_thread->stop();
	//m_emergethread.setRun(false);
	m_thread->wait();
	//m_emergethread.stop();

	if (m_netthr != nullptr) {
		m_netthr->stop();
		m_netthr->wait();
	}

	if (SNT != nullptr) {
		SNT->stop();
		SNT->wait();
	}

	///SSS->stop();
	SSE->stop();
	//SSS->wait();
	SSE->wait();

	ServersNetworkObject->StopNkillThreads();

	infostream<<"Server: Threads stopped"<<std::endl;
}

void Server::step(float dtime)
{
	// Limit a bit
	if (dtime > 2.0)
		dtime = 2.0;
	{
		MutexAutoLock lock(m_step_dtime_mutex);
		m_step_dtime += dtime;
	}
	// Throw if fatal error occurred in thread
	std::string async_err = m_async_fatal_error.get();
	if (!async_err.empty()) {
		if (!m_simple_singleplayer_mode) {
			if (!ServersNetworkObject->AreSlave)
				m_env->kickAllPlayers(SERVER_ACCESSDENIED_CRASH, g_settings->get("kick_msg_crash"), g_settings->getBool("ask_reconnect_on_crash"));
			else {
				//m_env->DoKickFromCrash(g_settings->get("kick_msg_crash"));
				NetworkPacket pkt(0x65, 0);
				pkt << (u8)ServersNetworkObject->QueryThisServerID();
				for (auto it = ClientDataTable.begin(); it != ClientDataTable.end(); ++it) {
					pkt << it->second->GetPlayerID_() << (u8) 0 << (u8) 0;
					Send(&pkt);
				}
			}
		}
		throw ServerError("AsyncErr: " + async_err);
	}
}

struct _aom_hlp { std::string a; std::string b; u16 id; u8 cmd; };

void Server::AsyncRunStep(bool initial_step)
{

	float dtime;
	{
		MutexAutoLock lock1(m_step_dtime_mutex);
		dtime = m_step_dtime;
	}

	{
		// Send blocks to clients
		SendBlocks(dtime);
	}

	if((dtime < 0.001) && !initial_step)
		return;

	ScopeProfiler sp(g_profiler, "Server::AsyncRunStep()", SPT_AVG);

	{
		MutexAutoLock lock1(m_step_dtime_mutex);
		m_step_dtime -= dtime;
	}

	/*
		Update uptime
	*/
	m_uptime_counter->increment(dtime);

	if (!ServersNetworkObject->AreSlave)
		handlePeerChanges();

	/*
		Update time of day and overall game time
	*/
	m_env->setTimeOfDaySpeed(g_settings->getFloat("time_speed"));

	/*
		Send to clients at constant intervals
	*/

	m_time_of_day_send_timer -= dtime;
	if (m_time_of_day_send_timer < 0.0) {
		m_time_of_day_send_timer = g_settings->getFloat("time_send_interval");
		u16 time = m_env->getTimeOfDay();
		float time_speed = g_settings->getFloat("time_speed");
		SendTimeOfDay(PEER_ID_INEXISTENT, time, time_speed);

		m_timeofday_gauge->set(time);
	}

	{
		MutexAutoLock lock(m_env_mutex);
		// Figure out and report maximum lag to environment
		float max_lag = m_env->getMaxLagEstimate();
		max_lag *= 0.9998; // Decrease slowly (about half per 5 minutes)
		if(dtime > max_lag){
			if(dtime > 0.1 && dtime > max_lag * 2.0)
				warningstream << "Server: Maximum lag peaked to " << dtime << " s" << std::endl;
			max_lag = dtime;
		}
		m_env->reportMaxLagEstimate(max_lag);
		// Step environment
		m_env->step(dtime);
	}

	static const float map_timer_and_unload_dtime = 2.92;
	if(m_map_timer_and_unload_interval.step(dtime, map_timer_and_unload_dtime))
	{
		MutexAutoLock lock(m_env_mutex);
		// Run Map's timers and unload unused data
		ScopeProfiler sp(g_profiler, "Server: map timer and unload");
		m_env->getMap().timerUpdate(map_timer_and_unload_dtime,
			g_settings->getFloat("server_unload_unused_data_timeout"),
			U32_MAX);
	}

	/*
		Listen to the admin chat, if available
	*/
	if (m_admin_chat) {
		if (!m_admin_chat->command_queue.empty()) {
			MutexAutoLock lock(m_env_mutex);
			while (!m_admin_chat->command_queue.empty()) {
				ChatEvent *evt = m_admin_chat->command_queue.pop_frontNoEx();
				handleChatInterfaceEvent(evt);
				delete evt;
			}
		}
		m_admin_chat->outgoing_queue.push_back(
			new ChatEventTimeInfo(m_env->getGameTime(), m_env->getTimeOfDay()));
	}

	/*
		Do background stuff
	*/

	/* Transform liquids */

	m_clients.step(dtime);

	// increase/decrease lag gauge gradually
	if (m_lag_gauge->get() > dtime) {
		m_lag_gauge->decrement(dtime/3);
	} else {
		m_lag_gauge->increment(dtime/3);
	}
#if USE_CURL
	// send masterserver announce
	{
		float &counter = m_masterserver_timer;
		if (!isSingleplayer() && (!counter || counter >= 60.0) &&
				g_settings->getBool("server_announce")) {
			ServerList::sendAnnounce(counter ? ServerList::AA_UPDATE :
						ServerList::AA_START,
					m_bind_addr.getPort(),
					m_clients.getPlayerNames(),
					m_uptime_counter->get(),
					m_env->getGameTime(),
					m_lag_gauge->get(),
					m_gamespec.id,
					Mapgen::getMapgenName(m_emerge->mgparams->mgtype),
					m_modmgr->getMods(),
					m_dedicated);
			counter = 0.01;
		}
		counter += dtime;
	}
#endif

	/*
		Check added and deleted active objects
	*/
	{
		//infostream<<"Server: Checking added and deleted active objects"<<std::endl;
		MutexAutoLock envlock(m_env_mutex);

		if (!ServersNetworkObject->AreSlave) {
			m_clients.lock();
			const RemoteClientMap &clients = m_clients.getClientList();
			ScopeProfiler sp(g_profiler, "Server: update objects within range");

			m_player_gauge->set(clients.size());
			for (const auto &client_it : clients) {
				RemoteClient *client = client_it.second;

				if (client->getState() < CS_DefinitionsSent)
					continue;

				RemotePlayer *player = m_env->getPlayer(client->peer_id);

				// This can happen if the client times out somehow
				if (!player)
					continue;

				if (player->to_other_server)
					continue;

				PlayerSAO *playersao = getPlayerSAO(client->peer_id);
				if (!playersao)
					continue;

				SendActiveObjectRemoveAdd(client, playersao);
			}
			m_clients.unlock();
		} else {
			ScopeProfiler sp(g_profiler, "Server: update objects within range");
			m_player_gauge->set(ClientDataTable.size());
			for (auto it = ClientDataTable.begin(); it != ClientDataTable.end(); ++it) {
				ClientDataHelper *client = it->second;
				if (!m_env->getPlayer(maskedu16(client->GetPlayerID_())))
					continue;
				PlayerSAO *playersao = getPlayerSAO(maskedu16(client->GetPlayerID_()));
				if (!playersao)
					continue;
				//if (client->countToInitialize <= 0)
					SendActiveObjectRemoveAdd_SLAVE(client, playersao);
				//else
				//	client->countToInitialize -= dtime;
			}
		}

		// Save mod storages if modified
		m_mod_storage_save_timer -= dtime;
		if (m_mod_storage_save_timer <= 0.0f) {
			m_mod_storage_save_timer = g_settings->getFloat("server_map_save_interval");
			int n = 0;
			for (std::unordered_map<std::string, ModMetadata *>::const_iterator
				it = m_mod_storages.begin(); it != m_mod_storages.end(); ++it) {
				if (it->second->isModified()) {
					it->second->save(getModStoragePath());
					n++;
				}
			}
			if (n > 0)
				infostream << "Saved " << n << " modified mod storages." << std::endl;
		}
	}

	/*
		Send object messages
	*/
	{
		MutexAutoLock envlock(m_env_mutex);
		ScopeProfiler sp(g_profiler, "Server: send SAO messages");

		// Key = object id
		// Value = data sent by object
		std::unordered_map<u16, std::vector<ActiveObjectMessage>*> buffered_messages;

		// Get active object messages from environment
		ActiveObjectMessage aom(0);
		u32 aom_count = 0;
		for(;;) {
			if (!m_env->getActiveObjectMessage(&aom))
				break;

			std::vector<ActiveObjectMessage>* message_list = nullptr;
			auto n = buffered_messages.find(aom.id);
			if (n == buffered_messages.end()) {
				message_list = new std::vector<ActiveObjectMessage>;
				buffered_messages[aom.id] = message_list;
			} else {
				message_list = n->second;
			}
			message_list->push_back(std::move(aom));
			aom_count++;
		}

		m_aom_buffer_counter->increment(aom_count);

		if (ServersNetworkObject->AreSlave) {
			//Send a direct AOM message to decompress in proxy on other thread
			NetworkPacket pkt(0x73, 0);
			u16 messages_count = 0;
			std::vector<_aom_hlp> messages_rel;
			std::vector<_aom_hlp> messages;
			std::string msg;
			//Compile and serialize every time the objs messages
			for (const auto &buffered_message : buffered_messages) {
				u16 id = buffered_message.first;
				ServerActiveObject *sao = m_env->getActiveObject(id);
				
				if (!sao)
					continue;
				/*
				[id -  u16]
				[reliable - u8]
				[lengh of 'message old_version' - u16]
				[lengh of 'message new_version' - u16]
				[message old_version]
				[message new_version]
				
				NOTE: IDs are replaced on proxy by an ID helper
				*/
				std::vector<ActiveObjectMessage> *list = buffered_message.second;
				for (const ActiveObjectMessage &aom : *list) {
					//Filter some things
					if ((aom.datastring[0] == AO_CMD_UPDATE_POSITION) && sao->getType() == ACTIVEOBJECT_TYPE_PLAYER) {
						//This will be handled by multiserver_env
						continue; //If Teleports, then the slave server will send a special packet for multiserver_env
					}
					if (aom.reliable) {
						_aom_hlp strs = {serializeString16(aom.datastring), serializeString16(aom.legacystring), aom.id, (u8)aom.datastring[0]};
						messages_rel.push_back(strs);
					} else {
						_aom_hlp strs = {serializeString16(aom.datastring), serializeString16(aom.legacystring), aom.id, (u8)aom.datastring[0]};
						messages.push_back(strs);
					}
				}
				messages_count++;
			}
			if (messages_count > 0) {
				//Prepare to send
				pkt << messages_count;
				//Send reliable, then unreliable
				for (const _aom_hlp MSG : messages_rel) {
					pkt << MSG.id;
					pkt << (u8)true;
					//pkt << (u32)MSG.b.size();
					//pkt << (u32)MSG.a.size();
					pkt << (u16)MSG.cmd; //to don't seek into the string directly
					//pkt.putRawString(MSG.b.c_str(), MSG.b.size());
					//pkt.putRawString(MSG.a.c_str(), MSG.a.size());
					
					pkt.putLongString(MSG.b);
					pkt.putLongString(MSG.a);
				}
				for (const _aom_hlp MSG : messages) {
					pkt << MSG.id;
					pkt << (u8)false;
					//pkt << (u32)MSG.b.size();
					//pkt << (u32)MSG.a.size();
					pkt << (u16)MSG.cmd; //to don't seek into the string directly
					//pkt.putRawString(MSG.b.c_str(), MSG.b.size());
					//pkt.putRawString(MSG.a.c_str(), MSG.a.size());
					pkt.putLongString(MSG.b);
					pkt.putLongString(MSG.a);
				}
				//Send
				Send(&pkt);
			}
		} else {
			m_clients.lock();
			const RemoteClientMap &clients = m_clients.getClientList();
			// Route data to every client
			std::string reliable_data, unreliable_data;
			for (const auto &client_it : clients) {
				reliable_data.clear();
				unreliable_data.clear();
				RemoteClient *client = client_it.second;
				PlayerSAO *player = getPlayerSAO(client->peer_id);
				RemotePlayer *p = m_env->getPlayer(client->peer_id);
				
				if (!p || p->to_other_server)
					continue;
				
				// Go through all objects in message buffer
				for (const auto &buffered_message : buffered_messages) {
					// If object does not exist or is not known by client, skip it
					u16 id = buffered_message.first;
					ServerActiveObject *sao = m_env->getActiveObject(id);
					if (!sao || client->m_known_objects.find(id) == client->m_known_objects.end())
						continue;

					// Get message list of object
					std::vector<ActiveObjectMessage>* list = buffered_message.second;
					// Go through every message
					for (const ActiveObjectMessage &aom : *list) {
						// Send position updates to players who do not see the attachment
						if (aom.datastring[0] == AO_CMD_UPDATE_POSITION) {
							if (sao->getId() == player->getId())
								continue;

							// Do not send position updates for attached players
							// as long the parent is known to the client
							ServerActiveObject *parent = sao->getParent();
							if (parent && client->m_known_objects.find(parent->getId()) !=
									client->m_known_objects.end())
								continue;
						}

						// Add full new data to appropriate buffer
						std::string &buffer = aom.reliable ? reliable_data : unreliable_data;
						char idbuf[2];
						writeU16((u8*) idbuf, aom.id);
						// u16 id
						// std::string data
						buffer.append(idbuf, sizeof(idbuf));
						if (client->net_proto_version >= 37 ||
								aom.legacystring.empty())
							buffer.append(serializeString16(aom.datastring));
						else
							buffer.append(serializeString16(aom.legacystring));
					}
				}
				/*
					reliable_data and unreliable_data are now ready.
					Send them.
				*/
				if (!reliable_data.empty()) {
					SendActiveObjectMessages(client->peer_id, reliable_data);
				}

				if (!unreliable_data.empty()) {
					SendActiveObjectMessages(client->peer_id, unreliable_data, false);
				}
			}
			m_clients.unlock();
		}

		// Clear buffered_messages
		for (auto &buffered_message : buffered_messages) {
			delete buffered_message.second;
		}
	}

	/*
		Send queued-for-sending map edit events.
	*/
	{
		// We will be accessing the environment
		MutexAutoLock lock(m_env_mutex);

		// Don't send too many at a time
		//u32 count = 0;

		// Single change sending is disabled if queue size is not small
		bool disable_single_change_sending = false;
		if(m_unsent_map_edit_queue.size() >= 4)
			disable_single_change_sending = true;

		int event_count = m_unsent_map_edit_queue.size();

		// We'll log the amount of each
		Profiler prof;

		std::list<v3s16> node_meta_updates;

		while (!m_unsent_map_edit_queue.empty()) {
			MapEditEvent* event = m_unsent_map_edit_queue.front();
			m_unsent_map_edit_queue.pop();

			// Players far away from the change are stored here.
			// Instead of sending the changes, MapBlocks are set not sent
			// for them.
			std::unordered_set<u16> far_players;

			switch (event->type) {
			case MEET_ADDNODE:
			case MEET_SWAPNODE:
				prof.add("MEET_ADDNODE", 1);
				sendAddNode(event->p, event->n, &far_players, disable_single_change_sending ? 5 : 30, event->type == MEET_ADDNODE);
				break;
			case MEET_REMOVENODE:
				prof.add("MEET_REMOVENODE", 1);
				sendRemoveNode(event->p, &far_players, disable_single_change_sending ? 5 : 30);
				break;
			case MEET_BLOCK_NODE_METADATA_CHANGED: {
				prof.add("MEET_BLOCK_NODE_METADATA_CHANGED", 1);
				if (!event->is_private_change) {
					// Don't send the change yet. Collect them to eliminate dupes.
					node_meta_updates.remove(event->p);
					node_meta_updates.push_back(event->p);
				}

				if (MapBlock *block = m_env->getMap().getBlockNoCreateNoEx(
						getNodeBlockPos(event->p))) {
					block->raiseModified(MOD_STATE_WRITE_NEEDED,
						MOD_REASON_REPORT_META_CHANGE);
				}
				break;
			}
			case MEET_OTHER:
				prof.add("MEET_OTHER", 1);
				for (const v3s16 &modified_block : event->modified_blocks) {
					m_clients.markBlockposAsNotSent(modified_block);
				}
				break;
			default:
				prof.add("unknown", 1);
				warningstream << "Server: Unknown MapEditEvent "
						<< ((u32)event->type) << std::endl;
				break;
			}

			/*
				Set blocks not sent to far players
			*/
			if (!far_players.empty()) {
				// Convert list format to that wanted by SetBlocksNotSent
				std::map<v3s16, MapBlock*> modified_blocks2;
				for (const v3s16 &modified_block : event->modified_blocks) {
					modified_blocks2[modified_block] =
							m_env->getMap().getBlockNoCreateNoEx(modified_block);
				}

				// Set blocks not sent LLK
				if (!ServersNetworkObject->AreSlave) {
					for (const u16 far_player : far_players) {
						if (RemoteClient *client = getClient(far_player))
							client->SetBlocksNotSent(modified_blocks2);
					}
				} else {
					for (const u16 far_player : far_players) {
						if (ClientDataTable.find(far_player) == ClientDataTable.end())
							continue;
						ClientDataHelper *client = ClientDataTable[far_player];
						if (client)
							client->SetBlocksNotSent(modified_blocks2);
					}
				}
			}

			delete event;
		}

		if (event_count >= 5) {
			infostream << "Server: MapEditEvents:" << std::endl;
			prof.print(infostream);
		} else if (event_count != 0) {
			verbosestream << "Server: MapEditEvents:" << std::endl;
			prof.print(verbosestream);
		}

		// Send all metadata updates
		if (node_meta_updates.size())
			sendMetadataChanged(node_meta_updates);
	}

	/*
		Trigger emergethread (it somehow gets to a non-triggered but
		bysy state sometimes)
	*/
	if (!ServersNetworkObject->AreSlave || m_definitions_done) {
		float &counter = m_emergethread_trigger_timer;
		counter += dtime;
		if (counter >= 2.0) {
			counter = 0.0;

			m_emerge->startThreads();
		}
	}

	// Save map, players and auth stuff
	{
		float &counter = m_savemap_timer;
		counter += dtime;
		static thread_local const float save_interval =
			g_settings->getFloat("server_map_save_interval");
		if (counter >= save_interval) {
			counter = 0.0;
			MutexAutoLock lock(m_env_mutex);

			ScopeProfiler sp(g_profiler, "Server: map saving (sum)");

			// Save ban file
			if (m_banmanager->isModified()) {
				m_banmanager->save();
			}

			// Save changed parts of map
			m_env->getMap().save(MOD_STATE_WRITE_NEEDED);

			// Save players
			m_env->saveLoadedPlayers();

			// Save environment metadata
			m_env->saveMeta();
		}
	}
	
	//Process queued packets from clients
	{
		while (!QueuedPackets.empty()) {
			QueuedPacketsMTX.lock();
			NetworkPacket pkt = QueuedPackets.front();
			QueuedPackets.pop_front();
			QueuedPacketsMTX.unlock();
			ProcessData(&pkt);
		}
	}


	//Initialize queued players <Lua side>
	{
		
		while (!ToInitialize.empty()) {
			QueuedPlayersToInitialize.lock();
			PlayerInitializerSLV p = std::move(ToInitialize.front());
			QueuedPlayersToInitialize.unlock();
			ToInitialize.pop_front();
			s64 last_login;
			m_script->getAuth(p.NAME, nullptr, nullptr, &last_login);
			m_script->on_joinplayer(p.SAO, last_login);
		}
		
	}

	{
		PlayersToInitialize_MTX.lock();
		while (!PlayersToInitialize.empty()) {
			session_t p = PlayersToInitialize.front();
			InitializePlayer(p);
			PlayersToInitialize.pop_front();
		}
		PlayersToInitialize_MTX.unlock();
	}

	m_shutdown_state.tick(dtime, this);
}

//Some helpers for multiserver environment

bool Server::equalsPlayerIDwithPeerID(u16 PlayerID, session_t PeerID) {
	RemotePlayer *player = m_env->getPlayer(maskedu16(PlayerID));
	if (!player)
		return false;
	if (player->getPeerId() == PeerID)
		return true;
	return false;
}

u16 Server::getProtocolOfThisPeer(session_t PeerID) {
	m_clients.lock();
	try {
		RemoteClient* client = m_clients.lockedGetClientNoEx(PeerID);
		if (client) {
			return client->net_proto_version;
		} else {
			return 0;
		}
	} catch (std::exception &e) {
		m_clients.unlock();
		throw;
	}
	m_clients.unlock();
}

float Server::getRecommendedSendInterval() {
	return m_env->getSendRecommendedInterval();
}

void Server::DoSendToEveryone(NetworkPacket *pkt, NetworkPacket *legacy_pkt, u16 SID) {
	for (auto it = PlayerInEnvironmentToServer.begin(); it != PlayerInEnvironmentToServer.end(); ++it) {
		if (it->second == SID) {
			RemotePlayer *p = m_env->getPlayer(it->first);
			if (p) {
				if (p->protocol_version >= 37) {
					Send(it->first, pkt);
				} else {
					Send(it->first, legacy_pkt);
				}
			}
		}
	}
}

//may require a struct
void Server::QueueSendTo(session_t peer, NetworkPacket pkt, bool reliable) {
	pkt.setReliableOpt(reliable);
	PacketsDequeMTX.lock();
	pkt.setPeerID(peer);
	PacketsDeque.push_back(pkt);
	PacketsDequeMTX.unlock();
}

//Should connect over the network
void Server::MakeThisASlaveServer(u16 id) {
	ServersNetworkObject->AreSlave = true;
	//Make this server act like a client for the proxy; Only for sending contents, receiving contents are in Server::Receive()
	if (!ServersNetworkObject->ProxyAddressAdded)
		FATAL_ERROR("Invalid Address for proxy, may you have don't set the proxy address?");
	MAINSERVEURE->SetTimeoutMs(0);
	MAINSERVEURE->Connect(ServersNetworkObject->ProxyAddress); //Element ProxyAddress are defined in mods
	//NetworkPacket n_pkt(0x66, 1);
	//n_pkt << (u8)ServersNetworkObject->QueryThisServerID();
	//n_pkt << (u8)1; //gargabe_data
	//Send(&n_pkt);
	//sendDefinitions();
}

void Server::QueueProcessData(NetworkPacket pkt) {
	QueuedPacketsMTX.lock();
	QueuedPackets.push_back(pkt);
	QueuedPacketsMTX.unlock();
}

void Server::HandleIDforServer(NetworkPacket *pkt) {
	u16 ID__;
	u8 null;
	*pkt >> null >> ID__;
	ServersNetworkObject->SetTSID(ID__);
	actionstream << "Received command from a proxy serv: 0x66, setting THIS server as an proxy, ID: " << ID__ << std::endl;
}

void Server::QueueOnJoinPlayer(PlayerSAO *sao, const char *name) {
	QueuedPlayersToInitialize.lock();
	ToInitialize.emplace_back();
	auto &queuer = ToInitialize.back();
	queuer.SAO = sao;
	queuer.NAME = name;
	QueuedPlayersToInitialize.unlock();
}

void Server::sendDefinitions() {
	warningstream << "Sending definitions!" << std::endl;
	u16 s_id = ServersNetworkObject->QueryThisServerID();
	//Send nodedef
	NetworkPacket pkt(0x70, 1);
	pkt << (u8)s_id;
	
	//standard sending (server->client) but proxy
	
	std::ostringstream osx1(std::ios::binary);
	m_itemdef->serialize(osx1, 38);
	std::ostringstream osx2(std::ios::binary);
	compressZlib(osx1.str(), osx2);
	pkt.putLongString(osx2.str());
	
	std::ostringstream tmp_os(std::ios::binary);
	m_nodedef->serialize(tmp_os, 38);
	std::ostringstream tmp_os2(std::ios::binary);
	compressZlib(tmp_os.str(), tmp_os2);
	pkt.putLongString(tmp_os2.str());
	
	Send(&pkt);
}

void Server::sendUpdatePlayerSaoList(u16 playerid, std::unordered_map<u16, u16> playerlistNsao) {
	NetworkPacket pkt(0x69, 0);
	pkt << playerid << playerlistNsao.size();
	for (auto it = playerlistNsao.begin(); it != playerlistNsao.end(); it++) {
		pkt << it->first << it->second; //first=PlayerID, second=PlayerSAO (depends on every player)
	}
	Send(&pkt);
}

void Server::HandleProxyCommand(NetworkPacket *pkt) {
	//This will handle this commands:
	/*
	TOSERVER_GOT_DISCONNECT
	TOSERVER_GOT_CONNECT
	0x95: Handle reserve player sao ID
	0x96: Handle 'SendMeItemDefinitions'
	0x97: Handle 'SendMeNodeDefinitions'
	0x98: Handle 'JumpDefinitionsAtNodeDef'
	0x99: Handle 'JumpDefinitionsAtItemDef'
	*/
	u16 CMD = pkt->getCommand();
	switch (CMD) {
		case 0x110: {
			//Server requires media data raw
			// Read data
			u16 count = 0;
			*pkt >> count;
			
			std::ostringstream toserv(std::ios_base::binary);
			
			for (u16 i = 0; i < count; i++) {
				std::string name;
				*pkt >> name;
				std::string tpath = m_media[name].path;
				std::ifstream fis(tpath.c_str(), std::ios_base::binary);
				if(!fis.good()){
					errorstream<<"Server[External]::0x110(): Could not open \""<<tpath<<"\" for reading"<<std::endl;
					continue;
				}
				std::ostringstream tmp_os(std::ios_base::binary);
				bool bad = false;
				for(;;) {
					char buf[1024];
					fis.read(buf, 1024);
					std::streamsize len = fis.gcount();
					tmp_os.write(buf, len);
					if(fis.eof())
						break;
					if(!fis.good()) {
						bad = true;
						break;
					}
				}
				if (bad) {
					errorstream<<"Server[External]::0x110(): Failed to read \""<<name<<"\""<<std::endl;
					continue;
				}
				
				toserv << serializeString16(name);
				toserv << serializeString64(tmp_os.str());
			}
			
			NetworkPacket to_send(0x75, 0);
			to_send << (u8)ServersNetworkObject->QueryThisServerID();
			to_send << count;
			to_send.putLongString(toserv.str());
			Send(&to_send);
		}
		case 0x116: {
			//Server requires media data sums
			// Make packet
			NetworkPacket pkt2(0x74, 0);
			pkt2 << (u8)ServersNetworkObject->QueryThisServerID();
			u16 media_sent = 0;
			std::string lang_suffix;
			//lang_suffix.append(".").append(lang_code).append(".tr");
			for (const auto &i : m_media) {
				//if (str_ends_with(i.first, ".tr") && !str_ends_with(i.first, lang_suffix))
				//	continue;
				//if (str_ends_with(i.first, ".tr.e") && !str_ends_with(i.first, lang_suffix + ".e"))
				//	continue;
				media_sent++;
			}
			pkt2 << (u16)m_media.size();
			for (const auto &i : m_media) {
				//if (str_ends_with(i.first, ".tr") && !str_ends_with(i.first, lang_suffix))
				//	continue;
				//if (str_ends_with(i.first, ".tr.e") && !str_ends_with(i.first, lang_suffix + ".e"))
				//	continue;
				pkt2 << i.first;
				pkt2 << i.second.sha1_digest;
			}
			Send(&pkt2);
			verbosestream << "Server: Announcing files to proxy: count=" << media_sent << " size=" << pkt2.getSize() << std::endl;
			return;
		}
		case 0x99: { //Proxy requires the data of definitions
			//sendDefinitions();
			return;
		}
		case 0x98: { //Perform a update to ItemDefinitions and NodeDefinitions
			//handleCommand_JumpDefinitions(pkt); //useless
			//verbosestream << "Received NodeDefinitions to update" << std::endl;
			//std::istringstream tmp_is(pkt->readLongString(), std::ios::binary);
			//std::ostringstream tmp_os;
			//decompressZlib(tmp_is, tmp_os);
			//std::istringstream tmp_is2(tmp_os.str());
			//m_nodedef->deSerialize(tmp_is2);
			
			/*m_nodedef->setNodeRegistrationStatus(true);
			// Perform pending node name resolutions
			m_nodedef->runNodeResolveCallbacks();
			// unmap node names in cross-references
			m_nodedef->resolveCrossrefs();
			// init the recipe hashes to speed up crafting
			m_craftdef->initHashes(this);
			m_emerge->initMapgens(m_startup_server_map->getMapgenParams());
			*/
			
			MutexAutoLock envlock(m_env_mutex);
			
			DEFINITIONS_EXECUTION.lock();
			
			//Copy all nodes which we will edit
			u16 count = 0;
			*pkt >> count;
			for (u16 i = 0; i < count; i++) {
				u16 original_id, new_id = 0;
				*pkt >> original_id >> new_id;
				Jumper[original_id] = new_id;
				verbosestream << FUNCTION_NAME << ": ToCopy: " << original_id << " | newID: " << new_id << std::endl;
			}
			
			//content_t fuku = 290348984;
			//std::cout << fuku << std::endl;
			//return;
			
			
			//Now we do changes
			/*
			for (auto it = OriginaltoNew.begin(); it != OriginaltoNew.end(); it++) {
				if (to_copy.find(it->first) == to_copy.end()) {
					warningstream << "Invalid idx" << std::endl;
					continue;
				}
				if (to_copy[it->first].name == "")
					continue; //frequent, FIXME
				m_nodedef->set(to_copy[it->first].name, to_copy[it->first], it->second);
			}*/
			
			//Here we initialize the big machine
			m_nodedef->updateAliases(m_itemdef);
			m_nodedef->setNodeRegistrationStatus(true);
			// Perform pending node name resolutions
			m_nodedef->runNodeResolveCallbacks();
			// unmap node names in cross-references
			m_nodedef->resolveCrossrefs();
			// init the recipe hashes to speed up crafting
			m_craftdef->initHashes(this);
			
			m_emerge->initMapgens(m_startup_server_map->getMapgenParams());
			
			m_definitions_done = true;
			
			DEFINITIONS_EXECUTION.unlock();
			
			return;
		}
		case 0x97: { //PlayerPOS compressed
			handleCommand_PlayerPosAdvanced(pkt);
			return;
		}
		case 0x59: {
			handleCommand_GotConnect(pkt);
			return;
		}
		case 0x58: {
			handleCommand_GotDisconnect(pkt);
			return;
		}
		default: {
			warningstream << "Unknown command base: " << CMD << std::endl;
			return;
		}
	}
}

//LLX1091: TODO: This need to be ran in a separate thread
//Supossing this running in a different thread
void Server::Receive()
{
	NetworkPacket pkt;
	if (!SocketConn) {
		session_t peer_id;
		bool first = true;
		u16 packet_count = 0;
		std::unordered_map<session_t, u16> peers_id;
		for (;;) {
			pkt.clear();
			peer_id = 0;
			try {
				/*
					In the first iteration *wait* for a packet, afterwards process
					all packets that are immediately available (no waiting).
				*/
				if (first) {
					m_con->Receive(&pkt);
					first = false;
				} else {
					if (!m_con->TryReceive(&pkt))
						return;
				}

				peer_id = pkt.getPeerId();

				//If it are a command without name, receive it and leave, as the example below this line
				/*if (pkt.getCommand() == 0x67) {
					SeenSlaveServers[peer_id] = true;
					actionstream << "Received 'hi' from a slave server, sending back a signal for ID completion" << std::endl;
					NetworkPacket n_pkt(0x66, 0);
					n_pkt <<
					return;
				}*/

				//DDOS check


				if (ServersNetworkObject->IsThisAProxy) {
					//May check if it are an slave [If the proxy has not a independient address]
					if (!ServersNetworkObject->IndependientIPaddress) {
						auto it = SeenSlaveServers.find(peer_id);
						if (it != SeenSlaveServers.end()) {
							u16 ID = SeenSlaveServersIDS[peer_id];
							ServersNetworkObject->PacketsQueue[ID]->QueuePacket(pkt); //make queue
							infostream << "Queued packet for server ID: " << ID << std::endl;
							return;
						}
					}

					if ((peers_id[peer_id] > 20) || ((peers_id[peer_id] > 5) && (pkt.getSize() > 1600))) { //The packet size should not be more than 1900bytes and should not be sent too quickly
						//If the 'hacker' was ddosing the server, send 4500 packets with 900bytes, fuck u Maintainer!, then, disconnect the peer and hard-ban the ip
						goto _ON_BEING_ATTACKED;
					}

					if (SessionsStatus[peer_id]) {
						//u16 PID = SessionsToU16.at(peer_id);
						u16 ServID = PlayerInEnvironmentToServer[peer_id];
						try {
							ServersNetworkObject->RedirectPacket(&pkt, ServID); //Redirect packet if player are playing in a separate server
						} catch (const FailureOnIndexingARegisteredServer &e) {
							errorstream << "RedirectPacket(): " << e.what() << std::endl;
						}
						//infostream << "Redirecting packet to server "<<ServID << std::endl;
						return;
					}
				}

				_ON_BEING_ATTACKED: {
					if ((peers_id[peer_id] > 20) || ((peers_id[peer_id] > 5) && (pkt.getSize() > 1600))) {
						m_con->DisconnectPeer(peer_id);
					}
				}


				if (pkt.getCommand() >= 0x57) {
					actionstream << "Got proxy command: " << pkt.getCommand() << std::endl;
					HandleProxyCommand(&pkt);
					return;
				}

				m_packet_recv_counter->increment();
				//When queued packets it will be executed in AsyncRunStep
				if (!ServersNetworkObject->AreSlave)
					QueueProcessData(pkt);
				else
					ProcessData(&pkt);
				m_packet_recv_processed_counter->increment();
			} catch (const con::InvalidIncomingDataException &e) {
				infostream << "Server::Receive(): InvalidIncomingDataException: what()="
						<< e.what() << std::endl;
			} catch (const SerializationError &e) {
				infostream << "Server::Receive(): SerializationError: what()="
						<< e.what() << std::endl;
			} catch (const ClientStateError &e) {
				errorstream << "ProcessData: peer=" << peer_id << " what()="
						<< e.what() << std::endl;
				DenyAccess(peer_id, SERVER_ACCESSDENIED_UNEXPECTED_DATA);
			} catch (const con::NoIncomingDataException &e) {
				return;
			}
		}
	} else {
		try {
			NetworkPacket pkt = m_netthr->getPacket();
			if (pkt.getCommand() != 0x0) {
				if (pkt.getCommand() >= 0x57) {
					actionstream << "Got proxy command: " << pkt.getCommand() << std::endl;
					HandleProxyCommand(&pkt);
					return;
				}
				m_packet_recv_counter->increment();
				ProcessData(&pkt);
				m_packet_recv_processed_counter->increment();
			}
		} catch (const SerializationError &e) {
			warningstream << "Server::Receive(): SerializationError: what()=" << e.what() << std::endl;
		}
	}
}

PlayerSAO* Server::StageTwoClientInit(session_t peer_id)
{
	std::string playername;
	PlayerSAO *playersao = NULL;
	m_clients.lock();
	try {
		RemoteClient* client = m_clients.lockedGetClientNoEx(peer_id, CS_InitDone);
		if (client) {
			playername = client->getName();
			playersao = emergePlayer(playername.c_str(), peer_id, client->net_proto_version);
		}
	} catch (std::exception &e) {
		m_clients.unlock();
		throw;
	}
	m_clients.unlock();

	RemotePlayer *player = m_env->getPlayer(playername.c_str());

	// If failed, cancel
	if (!playersao || !player) {
		if (player && player->getPeerId() != PEER_ID_INEXISTENT) {
			actionstream << "Server: Failed to emerge player \"" << playername
					<< "\" (player allocated to an another client)" << std::endl;
			DenyAccess(peer_id, SERVER_ACCESSDENIED_ALREADY_CONNECTED);
		} else {
			errorstream << "Server: " << playername << ": Failed to emerge player"
					<< std::endl;
			DenyAccess(peer_id, SERVER_ACCESSDENIED_SERVER_FAIL);
		}
		return nullptr;
	}

	/*
		Send complete position information
	*/
	SendMovePlayer(peer_id);

	// Send privileges
	SendPlayerPrivileges(peer_id);

	// Send inventory formspec
	SendPlayerInventoryFormspec(peer_id);

	// Send inventory
	SendInventory(playersao, false);

	// Send HP or death screen
	if (playersao->isDead())
		SendDeathscreen(peer_id, false, v3f(0,0,0));
	else
		SendPlayerHPOrDie(playersao,
				PlayerHPChangeReason(PlayerHPChangeReason::SET_HP));

	// Send Breath
	SendPlayerBreath(playersao);

	/*
		Print out action
	*/
	{
		Address addr = getPeerAddress(player->getPeerId());
		std::string ip_str = addr.serializeString();
		const std::vector<std::string> &names = m_clients.getPlayerNames();

		actionstream << player->getName() << " [" << ip_str << "] joins game. List of players: ";

		for (const std::string &name : names) {
			actionstream << name << " ";
		}

		actionstream << player->getName() <<std::endl;
	}
	return playersao;
}

//executed when queued [ONLY ON PROXY]
void Server::InitializePlayer(session_t pid) {
	RemotePlayer *player = m_env->getPlayer(pid);
	PlayerSAO *playersao = player->getPlayerSAO(); //
	playersao->finalize(player, getPlayerEffectivePrivs(player->getName()));
	SendMovePlayer(pid);
	SendPlayerPrivileges(pid);
	SendPlayerInventoryFormspec(pid);
	SendInventory(playersao, false);
	if (playersao->isDead())
		SendDeathscreen(pid, false, v3f(0,0,0));
	else
		SendPlayerHPOrDie(playersao, PlayerHPChangeReason(PlayerHPChangeReason::SET_HP));
	SendPlayerBreath(playersao);
	const std::vector<std::string> &players = m_clients.getPlayerNames();
	std::vector<std::string> PLIST; //filter players
	for (const std::string &player: players) {
		RemotePlayer *p = m_env->getPlayer(player.c_str());
		if (!p->OnServer->IsApplied) {
			PLIST.push_back(player);
		} else {
			warningstream << "Skipping player " << player << " as it are in other realm" << std::endl;
		}
	}
	NetworkPacket list_pkt(TOCLIENT_UPDATE_PLAYER_LIST, 0);
	list_pkt << (u8) PLAYER_LIST_INIT << (u16) players.size();
	for (std::string player : PLIST) {
		list_pkt << player;
	}
	m_clients.send(pid, 0, &list_pkt, true);
	s64 last_login;
	m_script->getAuth(playersao->getPlayer()->getName(), nullptr, nullptr, &last_login);
	m_script->on_joinplayer(playersao, last_login);
	{
		Address addr = getPeerAddress(player->getPeerId());
		std::string ip_str = addr.serializeString();
		const std::vector<std::string> &names = m_clients.getPlayerNames();
		actionstream << player->getName() << " [" << ip_str << "] joins game. List of players: ";
		for (const std::string &name : names) {
			actionstream << name << " ";
		}
		actionstream << player->getName() <<std::endl;
	}
	player->OnServer->IsApplied = false;
	SessionsStatus[pid] = false;
	PlayerInEnvironmentToServer[pid] = (u16)0;
	ActivePlayersU16[pid] = true;
}

void Server::QueuePlayerToInitialize(session_t p) {
	PlayersToInitialize_MTX.lock();
	PlayersToInitialize.push_back(p);
	PlayersToInitialize_MTX.unlock();
}

//Delete client
void Server::DeletePlayer(u16 p) {
	DeleteClientU16(p, CDR_LEAVE);
}

//handleCommand_GotConnect
PlayerSAO* Server::InitClientByMainServer(u16 ID_, ClientDataHelper *client) {
	std::string playername = client->getName();
	maskedu16 ID = maskedu16(ID_);
	PlayerSAO *playersao = emergePlayer(playername.c_str(), ID, client->net_proto_version);

	RemotePlayer *player = m_env->getPlayer(playername.c_str());

	// If failed, cancel
	if (!playersao || !player) {
		if (player && player->player_id != 0) {
			actionstream << "Server: Failed to emerge player \"" << playername << "\" (player allocated to an another client)" << std::endl;
			//DenyAccess(peer_id, SERVER_ACCESSDENIED_ALREADY_CONNECTED);
		} else {
			errorstream << "Server: " << playername << ": Failed to emerge player" << std::endl;
			//DenyAccess(peer_id, SERVER_ACCESSDENIED_SERVER_FAIL);
		}
		return nullptr;
	}

	player->OnServer->InSlaveServerISPLAYINGHERE = true;
	player->OnServer->IsApplied = true;
	player->OnServer->ID = ServersNetworkObject->QueryThisServerID();
	player->player_id = ID_;
	player->ID_OF_TRUE_SAO = client->IdInSlave;

	/*
		Send complete position information
	*/
	SendMovePlayer(ID);

	// Send privileges
	SendPlayerPrivileges(ID);

	// Send inventory formspec
	SendPlayerInventoryFormspec(ID);

	// Send inventory
	SendInventory(playersao, false);

	// Send HP or death screen
	if (playersao->isDead())
		SendDeathscreen(ID, false, v3f(0,0,0));
	else
		SendPlayerHPOrDie(playersao, PlayerHPChangeReason(PlayerHPChangeReason::SET_HP));

	// Send Breath
	SendPlayerBreath(playersao);

	/*
		Print out action
	*/
	{
		actionstream << player->getName() << " [" << playername << "|PROXY|ID<" << client->IdInSlave << ">,sID<" << playersao->getId() << ">] joins game." << std::endl;
	}
	
	std::wstring message = L"Welcome!";
	
	SendChatMessage(ID, ChatMessage(CHATMESSAGE_TYPE_ANNOUNCE, message));
	
	PlayersIDsMUTEX.lock();
	ClientDataTable[ID.value] = client;
	
	MasterServerUniqueIDS *spr = new MasterServerUniqueIDS(client->IdInSlave); //playersao->getId()
	
	spr->SET_SAO_ID(playersao->getId());
	
	ClientDataTable[player->player_id]->SuperIDs = spr;
	PlayersIDsMUTEX.unlock();
	
	//Send to main server to delete the SAO in proxy [SAO are not deleted when sendConnect used, because the client might crash with SIGSEGV <tested>]
	NetworkPacket r_pkt(0x68, 3);
	r_pkt << (u8)ServersNetworkObject->QueryThisServerID() << ID_; // []X []y []y
	Send(&r_pkt);
	
	
	
	return playersao;
}

inline void Server::handleCommand(NetworkPacket *pkt)
{
	//This should handle player interactions with the server...
	const ToServerCommandHandler &opHandle = toServerCommandTable[pkt->getCommand()];
	(this->*opHandle.handler)(pkt);
}

void Server::ProcessData(NetworkPacket *pkt)
{
	// Environment is locked first.
	MutexAutoLock envlock(m_env_mutex);

	ScopeProfiler sp(g_profiler, "Server: Process network packet (sum)");
	u32 peer_id = pkt->getPeerId();

	if (ServersNetworkObject->AreSlave) {
		ToServerCommand command = (ToServerCommand) pkt->getCommand();
		if (command >= TOSERVER_NUM_MSG_TYPES) {
			infostream << "Server: Ignoring unknown command " << command << std::endl;
			return;
		}
		handleCommand(pkt);
		return;
	}

	try {
		Address address = getPeerAddress(peer_id);
		std::string addr_s = address.serializeString();
		
		// FIXME: Isn't it a bit excessive to check this for every packet?
		if (m_banmanager->isIpBanned(addr_s)) {
			std::string ban_name = m_banmanager->getBanName(addr_s);
			infostream << "Server: A banned client tried to connect from "
					<< addr_s << "; banned name was " << ban_name << std::endl;
			DenyAccess(peer_id, SERVER_ACCESSDENIED_CUSTOM_STRING,
				"Your IP is banned. Banned name was " + ban_name);
			return;
		}
	} catch (con::PeerNotFoundException &e) {
		/*
		 * no peer for this packet found
		 * most common reason is peer timeout, e.g. peer didn't
		 * respond for some time, your server was overloaded or
		 * things like that.
		 */
		infostream << "Server::ProcessData(): Canceling: peer "
				<< peer_id << " not found" << std::endl;
		return;
	}
	
	

	try {
		ToServerCommand command = (ToServerCommand) pkt->getCommand();

		Address address = getPeerAddress(peer_id);
		std::string addr_s = address.serializeString();

		// Command must be handled into ToServerCommandHandler
		if (command >= TOSERVER_NUM_MSG_TYPES) {
			infostream << "Server: Ignoring unknown command "
					 << command << std::endl;
			return;
		}


		if (toServerCommandTable[command].state == TOSERVER_STATE_NOT_CONNECTED) {
			handleCommand(pkt);
			return;
		}

		u8 peer_ser_ver = getClient(peer_id, CS_InitDone)->serialization_version;

		if(peer_ser_ver == SER_FMT_VER_INVALID) {
			errorstream << "Server::ProcessData(): Cancelling: Peer"
					" serialization format invalid or not initialized."
					" Skipping incoming command=" << command << std::endl;
			return;
		}

		/* Handle commands related to client startup */
		if (toServerCommandTable[command].state == TOSERVER_STATE_STARTUP) {
			handleCommand(pkt);
			return;
		}

		if (m_clients.getClientState(peer_id) < CS_Active) {
			if (command == TOSERVER_PLAYERPOS) return;

			errorstream << "Got packet command: " << command << " for peer id "
					<< peer_id << " but client isn't active yet. Dropping packet "
					<< std::endl;
			return;
		}

		RemoteClient *client = getClient(peer_id);
		if (client)
			pkt->setProtocolVersion(client->net_proto_version);
		handleCommand(pkt);
	} catch (SendFailedException &e) {
		errorstream << "Server::ProcessData(): SendFailedException: "
				<< "what=" << e.what()
				<< std::endl;
	} catch (PacketError &e) {
		actionstream << "Server::ProcessData(): PacketError: "
				<< "what=" << e.what() << ", command=" << pkt->getCommand() // TODO: REMOVE COMMAND=
				<< std::endl;
	}
}

void Server::setTimeOfDay(u32 time)
{
	m_env->setTimeOfDay(time);
	m_time_of_day_send_timer = 0;
}

void Server::onMapEditEvent(const MapEditEvent &event)
{
	if (m_ignore_map_edit_events_area.contains(event.getArea()))
		return;

	m_unsent_map_edit_queue.push(new MapEditEvent(event));
}

void Server::SetBlocksNotSent(std::map<v3s16, MapBlock *>& block)
{
	std::vector<session_t> clients = m_clients.getClientIDs();
	m_clients.lock();
	// Set the modified blocks unsent for all the clients
	for (const session_t client_id : clients) {
			if (RemoteClient *client = m_clients.lockedGetClientNoEx(client_id))
				client->SetBlocksNotSent(block);
	}
	m_clients.unlock();
}

void Server::peerAdded(con::Peer *peer)
{
	verbosestream<<"Server::peerAdded(): peer->id=" <<peer->id<<std::endl;

	if (!ServersNetworkObject->AreSlave)
		m_peer_change_queue.push(con::PeerChange(con::PEER_ADDED, peer->id, false));
}

void Server::deletingPeer(con::Peer *peer, bool timeout)
{
	verbosestream<<"Server::deletingPeer(): peer->id=" <<peer->id<<", timeout="<<timeout<<std::endl;

	//Should check!

	m_clients.event(peer->id, CSE_Disconnect);
	m_peer_change_queue.push(con::PeerChange(con::PEER_REMOVED, peer->id, timeout));
}

bool Server::getClientConInfo(session_t peer_id, con::rtt_stat_type type, float* retval)
{
	*retval = m_con->getPeerStat(peer_id,type);
	return *retval != -1;
}

bool Server::getClientInfo(session_t peer_id, ClientInfo &ret)
{
	m_clients.lock();
	RemoteClient* client = m_clients.lockedGetClientNoEx(peer_id, CS_Invalid);

	if (!client) {
		m_clients.unlock();
		return false;
	}

	ret.state = client->getState();
	ret.addr = client->getAddress();
	ret.uptime = client->uptime();
	ret.ser_vers = client->serialization_version;
	ret.prot_vers = client->net_proto_version;

	ret.major = client->getMajor();
	ret.minor = client->getMinor();
	ret.patch = client->getPatch();
	ret.vers_string = client->getFullVer();
	ret.platform = client->getPlatform();
	ret.sysinfo = client->getSysInfo();

	ret.lang_code = client->getLangCode();

	m_clients.unlock();

	return true;
}

bool Server::getClientInfo(maskedu16 pid, ClientInfo &ret)
{
	if (ClientDataTable.find(pid.value) == ClientDataTable.end()) {
		return false;
	}

	ClientDataHelper* client = ClientDataTable[pid.value];

	

	ret.state = client->getState();
	ret.addr = client->getAddress();
	ret.uptime = client->uptime();
	ret.ser_vers = client->serialization_version;
	ret.prot_vers = client->net_proto_version;

	ret.major = client->getMajor();
	ret.minor = client->getMinor();
	ret.patch = client->getPatch();
	ret.vers_string = client->getFullVer();
	ret.platform = client->getPlatform();
	ret.sysinfo = client->getSysInfo();

	ret.lang_code = client->getLangCode();


	return true;
}

void Server::handlePeerChanges()
{
	while(!m_peer_change_queue.empty())
	{
		con::PeerChange c = m_peer_change_queue.front();
		m_peer_change_queue.pop();

		verbosestream<<"Server: Handling peer change: "
				<<"id="<<c.peer_id<<", timeout="<<c.timeout
				<<std::endl;

		switch(c.type)
		{
		case con::PEER_ADDED:
			m_clients.CreateClient(c.peer_id);
			break;

		case con::PEER_REMOVED:
			DeleteClient(c.peer_id, c.timeout?CDR_TIMEOUT:CDR_LEAVE);
			break;

		default:
			FATAL_ERROR("Invalid peer change event received!");
			break;
		}
	}
}

void Server::printToConsoleOnly(const std::string &text)
{
	if (m_admin_chat) {
		m_admin_chat->outgoing_queue.push_back(
			new ChatEventChat("", utf8_to_wide(text)));
	} else {
		std::cout << text << std::endl;
	}
}

//Called on queue system
void Server::Send(NetworkPacket *pkt, bool relative)
{
	if (ServersNetworkObject->IsThisAProxy)
		Send(pkt->getPeerId(), pkt, relative);
	else 
		SendToMainServer(pkt);
}

void Server::Send(NetworkPacket *pkt)
{
	if (ServersNetworkObject->IsThisAProxy)
		Send(pkt->getPeerId(), pkt);
	else 
		SendToMainServer(pkt);
}

#include <errno.h>

void Server::SendToMainServer(NetworkPacket *pkt) {
	if (!SocketConn) {
		NetCR->queuePacket(*pkt);
	} else {
		//Send a raw big packet
		std::vector<uint8_t> raw_data;
		raw_data.resize(2+pkt->getSize());
		writeU16(&raw_data[0], pkt->getCommand());
		memcpy(&raw_data[2], pkt->getU8Ptr(0), pkt->getSize()+2);
		if (send(m_netthr->m_parent_skt.load(), &raw_data[0], raw_data.size(), 0) == -1) {
			int err = errno;
			errorstream << FUNCTION_NAME << ": Sending packet failed! <" << err << "; " << strerror(err) << ">" << std::endl;
		}
	}
}

void Server::Send(session_t peer_id, NetworkPacket *pkt, bool relative)
{
	//verbosestream << "Used ::Send() with forced reliable option, channel: " << pkt->getChannel() << std::endl;
	m_clients.send(peer_id, pkt->didSetChannel() ? pkt->getChannel() : clientCommandFactoryTable[pkt->getCommand()].channel, pkt, relative);
}

void Server::Send(session_t peer_id, NetworkPacket *pkt)
{
	//verbosestream << "Used ::Send() with builtin reliable option" << std::endl;
	m_clients.send(peer_id, clientCommandFactoryTable[pkt->getCommand()].channel, pkt, clientCommandFactoryTable[pkt->getCommand()].reliable);
}

void Server::SendMovement(session_t peer_id, u16 protocol_version)
{
	std::ostringstream os(std::ios_base::binary);

	NetworkPacket pkt(TOCLIENT_MOVEMENT, 12 * sizeof(float), peer_id, protocol_version);

	pkt << g_settings->getFloat("movement_acceleration_default");
	pkt << g_settings->getFloat("movement_acceleration_air");
	pkt << g_settings->getFloat("movement_acceleration_fast");
	pkt << g_settings->getFloat("movement_speed_walk");
	pkt << g_settings->getFloat("movement_speed_crouch");
	pkt << g_settings->getFloat("movement_speed_fast");
	pkt << g_settings->getFloat("movement_speed_climb");
	pkt << g_settings->getFloat("movement_speed_jump");
	pkt << g_settings->getFloat("movement_liquid_fluidity");
	pkt << g_settings->getFloat("movement_liquid_fluidity_smooth");
	pkt << g_settings->getFloat("movement_liquid_sink");
	pkt << g_settings->getFloat("movement_gravity");

	Send(&pkt);
}

//must be used on principal initializing.
void Server::SendMovement(maskedu16 pid, u16 protocol_version)
{
	//i don't find any usage on the function that is below this line.
	//std::ostringstream os(std::ios_base::binary);

	NetworkPacket pkt(TOCLIENT_MOVEMENT, 3 + (12 * sizeof(float)), 0, protocol_version);
	
	pkt << (u8) ServersNetworkObject->QueryThisServerID();
	pkt << pid.value;
	
	pkt << g_settings->getFloat("movement_acceleration_default");
	pkt << g_settings->getFloat("movement_acceleration_air");
	pkt << g_settings->getFloat("movement_acceleration_fast");
	pkt << g_settings->getFloat("movement_speed_walk");
	pkt << g_settings->getFloat("movement_speed_crouch");
	pkt << g_settings->getFloat("movement_speed_fast");
	pkt << g_settings->getFloat("movement_speed_climb");
	pkt << g_settings->getFloat("movement_speed_jump");
	pkt << g_settings->getFloat("movement_liquid_fluidity");
	pkt << g_settings->getFloat("movement_liquid_fluidity_smooth");
	pkt << g_settings->getFloat("movement_liquid_sink");
	pkt << g_settings->getFloat("movement_gravity");

	Send(&pkt);
}

void Server::SendPlayerHPOrDie(PlayerSAO *playersao, const PlayerHPChangeReason &reason)
{
	if (!ServersNetworkObject->AreSlave) {
		if (playersao->isImmortal())
			return;
		session_t peer_id = playersao->getPeerID();
		bool is_alive = !playersao->isDead();
		if (is_alive)
			SendPlayerHP(peer_id);
		else
			DiePlayer(peer_id, reason);
	} else {
		if (playersao->isImmortal())
			return;
		u16 PID = playersao->getPlayerID();
		bool is_alive = !playersao->isDead();
		if (is_alive)
			SendPlayerHP(maskedu16(PID));
		else
			DiePlayer(maskedu16(PID), reason);
	}
}

void Server::SendHP(session_t peer_id, u16 hp)
{
	NetworkPacket pkt(TOCLIENT_HP, 1, peer_id);
	// Minetest 0.4 uses 8-bit integers for HPP.
	if (m_clients.getProtocolVersion(peer_id) >= 37) {
		pkt << hp;
	} else {
		u8 raw_hp = hp & 0xFF;
		pkt << raw_hp;
	}
	Send(&pkt);
}

void Server::SendHP(maskedu16 PID, u16 hp) {
	NetworkPacket pkt(TOCLIENT_HP, 1 + 2 + 2);
	pkt << (u8)ServersNetworkObject->QueryThisServerID();
	pkt << PID.value;
	pkt << hp;
	Send(&pkt);
}

void Server::SendBreath(session_t peer_id, u16 breath)
{
	NetworkPacket pkt(TOCLIENT_BREATH, 2, peer_id);
	pkt << (u16) breath;
	Send(&pkt);
}

void Server::SendBreath(maskedu16 pid, u16 breath)
{
	NetworkPacket pkt(TOCLIENT_BREATH, 2 + 2 + 1);
	pkt << (u8)ServersNetworkObject->QueryThisServerID();
	pkt << pid.value;
	pkt << (u16) breath;
	Send(&pkt);
}

void Server::SendAccessDenied(session_t peer_id, AccessDeniedCode reason,
		const std::string &custom_reason, bool reconnect)
{
	assert(reason < SERVER_ACCESSDENIED_MAX);

	NetworkPacket pkt(TOCLIENT_ACCESS_DENIED, 1, peer_id);
	pkt << (u8)reason;
	if (reason == SERVER_ACCESSDENIED_CUSTOM_STRING)
		pkt << custom_reason;
	else if (reason == SERVER_ACCESSDENIED_SHUTDOWN ||
			reason == SERVER_ACCESSDENIED_CRASH)
		pkt << custom_reason << (u8)reconnect;
	Send(&pkt);
}

void Server::SendDeathscreen(session_t peer_id, bool set_camera_point_target,
		v3f camera_point_target)
{
	NetworkPacket pkt(TOCLIENT_DEATHSCREEN, 1 + sizeof(v3f), peer_id);
	pkt << set_camera_point_target << camera_point_target;
	Send(&pkt);
}

void Server::SendDeathscreen(maskedu16 pid, bool set_camera_point_target, v3f camera_point_target)
{
	NetworkPacket pkt(TOCLIENT_DEATHSCREEN, 1 + sizeof(v3f) + 1 + 2);
	pkt << (u8)ServersNetworkObject->QueryThisServerID();
	pkt << pid.value;
	pkt << set_camera_point_target << camera_point_target;
	Send(&pkt);
}

void Server::SendItemDef(session_t peer_id,
		IItemDefManager *itemdef, u16 protocol_version)
{
	NetworkPacket pkt(TOCLIENT_ITEMDEF, 0, peer_id);

	/*
		u16 command
		u32 length of the next item
		zlib-compressed serialized ItemDefManager
	*/
	std::ostringstream tmp_os(std::ios::binary);
	itemdef->serialize(tmp_os, protocol_version);
	std::ostringstream tmp_os2(std::ios::binary);
	compressZlib(tmp_os.str(), tmp_os2);
	pkt.putLongString(tmp_os2.str());

	// Make data buffer
	verbosestream << "Server: Sending item definitions to id(" << peer_id
			<< "): size=" << pkt.getSize() << std::endl;

	Send(&pkt);
}

//TODO: Should we send this data to proxy server to jump definitions?
void Server::SendNodeDef(session_t peer_id,
	const NodeDefManager *nodedef, u16 protocol_version)
{
	NetworkPacket pkt(TOCLIENT_NODEDEF, 0, peer_id);

	/*
		u16 command
		u32 length of the next item
		zlib-compressed serialized NodeDefManager
	*/
	std::ostringstream tmp_os(std::ios::binary);
	nodedef->serialize(tmp_os, protocol_version);
	std::ostringstream tmp_os2(std::ios::binary);
	compressZlib(tmp_os.str(), tmp_os2);

	pkt.putLongString(tmp_os2.str());

	// Make data buffer
	verbosestream << "Server: Sending node definitions to id(" << peer_id
			<< "): size=" << pkt.getSize() << std::endl;

	Send(&pkt);
}

/*
	Non-static send methods
*/

void Server::SendInventory(PlayerSAO *sao, bool incremental)
{
	//errorstream << "nasty" << std::endl;
	if (ServersNetworkObject->AreSlave) {
		RemotePlayer *player = sao->getPlayer();

		// Do not send new format to old clients
		incremental &= player->protocol_version >= 38;

		UpdateCrafting(player);

		/*
			Serialize it
		*/

		u16 SrvID = ServersNetworkObject->QueryThisServerID();

		NetworkPacket pkt(TOCLIENT_INVENTORY, 2 + 1);

		pkt << (u8)SrvID;
		pkt << sao->getPlayerID();

		std::ostringstream os(std::ios::binary);
		sao->getInventory()->serialize(os, incremental);
		sao->getInventory()->setModified(false);
		player->setModified(true);

		const std::string &s = os.str();
		pkt.putRawString(s.c_str(), s.size());
		Send(&pkt);
	} else {
		RemotePlayer *player = sao->getPlayer();

		// Do not send new format to old clients
		incremental &= player->protocol_version >= 38;

		UpdateCrafting(player);

		/*
			Serialize it
		*/

		NetworkPacket pkt(TOCLIENT_INVENTORY, 0, sao->getPeerID());

		std::ostringstream os(std::ios::binary);
		sao->getInventory()->serialize(os, incremental);
		sao->getInventory()->setModified(false);
		player->setModified(true);

		const std::string &s = os.str();
		pkt.putRawString(s.c_str(), s.size());
		Send(&pkt);
	}
}

void Server::SendChatMessage(session_t peer_id, const ChatMessage &message)
{
	NetworkPacket legacypkt(TOCLIENT_CHAT_MESSAGE_OLD, 0, peer_id);
	legacypkt << message.message;

	NetworkPacket pkt(TOCLIENT_CHAT_MESSAGE, 0, peer_id);
	u8 version = 1;
	u8 type = message.type;
	pkt << version << type << message.sender << message.message << static_cast<u64>(message.timestamp);

	if (peer_id != PEER_ID_INEXISTENT) {
		RemotePlayer *player = m_env->getPlayer(peer_id);
		if (!player)
			return;

		if (player->protocol_version < 35)
			Send(&legacypkt);
		else
			Send(&pkt);
	} else {
		//HHH
		//m_clients.sendToAllCompat(&pkt, &legacypkt, 35);
		const RemoteClientMap &clients = m_clients.getClientList();
		// Route data to every client
		//MutexAutoLock envlock(m_env_mutex);
		m_clients.lock();
		for (const auto &client_it : clients) {
			RemoteClient *client = client_it.second;
			
			RemotePlayer *player = m_env->getPlayer(client->peer_id);
			
			if (!player)
				continue;
			
			if (player->OnServer->IsApplied)
				continue;
			
			if (client->net_proto_version >= 35) {
				pkt.setPeerID(client->peer_id);
				Send(&pkt);
			} else if (client->net_proto_version != 0) {
				legacypkt.setPeerID(client->peer_id);
				Send(&pkt);
			}
		}
		m_clients.unlock();
	}
}

void Server::SendChatMessage(maskedu16 PID, const ChatMessage &message) {
	//NetworkPacket legacypkt(TOCLIENT_CHAT_MESSAGE_OLD, 0, peer_id);
	//legacypkt << message.message;

	NetworkPacket pkt(TOCLIENT_CHAT_MESSAGE, 0);
	
	/*
		The format of the new TOCLIENT_CHAT_MESSAGE {Slave} are not the same as the old TOCLIENT_CHAT_MESSAGE:
		[0] CMD
		[1] u8 ID of the sender server
		[2] u16 ID of the player <If specified>
		[3] u8 Mode of chat message [0: Send to everyone in the slave server, 1: Send to a specific player]
		[4] the entire data :)
	*/
	
	u16 SrvID = ServersNetworkObject->QueryThisServerID();
	
	pkt << (u8)SrvID;
	pkt << PID.value; //If PID is 0 then always send, don't care
	pkt << (u8)(PID.value > 0); //If PID is valid, then send to a specific player
	
	u8 version = 1;
	u8 type = message.type;
	pkt << version << type << message.sender << message.message << static_cast<u64>(message.timestamp);

	Send(&pkt);
}


void Server::SendShowFormspecMessage(session_t peer_id, const std::string &formspec,
	const std::string &formname)
{
	NetworkPacket pkt(TOCLIENT_SHOW_FORMSPEC, 0, peer_id);
	if (formspec.empty()){
		//the client should close the formspec
		//but make sure there wasn't another one open in meantime
		const auto it = m_formspec_state_data.find(peer_id);
		if (it != m_formspec_state_data.end() && it->second == formname) {
			m_formspec_state_data.erase(peer_id);
		}
		pkt.putLongString("");
	} else {
		m_formspec_state_data[peer_id] = formname;
		RemotePlayer *player = m_env->getPlayer(peer_id);
		if (player && player->protocol_version < 37)
			pkt.putLongString(insert_formspec_prepend(formspec, player->formspec_prepend));
		else
			pkt.putLongString(formspec);
	}
	pkt << formname;

	Send(&pkt);
}

void Server::SendShowFormspecMessage(maskedu16 player_id, const std::string &formspec, const std::string &formname)
{
	NetworkPacket pkt(TOCLIENT_SHOW_FORMSPEC, 0);
	
	pkt << (u8)ServersNetworkObject->QueryThisServerID();
	pkt << player_id.value;
	if (formspec.empty()){
		//the client should close the formspec
		//but make sure there wasn't another one open in meantime
		const auto it = m_formspec_state_dataU16.find(player_id.value);
		if (it != m_formspec_state_dataU16.end() && it->second == formname) {
			m_formspec_state_dataU16.erase(player_id.value);
		}
		pkt.putLongString("");
	} else {
		m_formspec_state_dataU16[player_id.value] = formname;
		RemotePlayer *player = m_env->getPlayer(player_id);
		if (player && player->protocol_version < 37)
			pkt.putLongString(insert_formspec_prepend(formspec, player->formspec_prepend));
		else
			pkt.putLongString(formspec);
	}
	
	pkt << formname;

	Send(&pkt);
}

// Spawns a particle on peer with peer_id
void Server::SendSpawnParticle(session_t peer_id, u16 protocol_version,
	const ParticleParameters &p)
{
	static thread_local const float radius =
			g_settings->getS16("max_block_send_distance") * MAP_BLOCKSIZE * BS;

	if (peer_id == PEER_ID_INEXISTENT) {
		std::vector<session_t> clients = m_clients.getClientIDs();
		const v3f pos = p.pos * BS;
		const float radius_sq = radius * radius;

		for (const session_t client_id : clients) {
			RemotePlayer *player = m_env->getPlayer(client_id);
			if (!player)
				continue;

			PlayerSAO *sao = player->getPlayerSAO();
			if (!sao)
				continue;

			// Do not send to distant clients
			if (sao->getBasePosition().getDistanceFromSQ(pos) > radius_sq)
				continue;

			SendSpawnParticle(client_id, player->protocol_version, p);
		}
		return;
	}
	assert(protocol_version != 0);

	NetworkPacket pkt(TOCLIENT_SPAWN_PARTICLE, 0, peer_id, protocol_version);

	{
		// NetworkPacket and iostreams are incompatible... OK
		std::ostringstream oss(std::ios_base::binary);
		p.serialize(oss, protocol_version);
		pkt.putRawString(oss.str());
	}

	Send(&pkt);
}

void Server::SendSpawnParticle(maskedu16 playerid, u16 protocol_version, const ParticleParameters &pp) {
	static thread_local const float radius =
			g_settings->getS16("max_block_send_distance") * MAP_BLOCKSIZE * BS;

	if (playerid.value == 0) {
		std::vector<RemotePlayer *> clients = m_env->getPlayers();
		const v3f pos = pp.pos * BS;
		const float radius_sq = radius * radius;

		for (RemotePlayer *p : clients) {

			PlayerSAO *sao = p->getPlayerSAO();
			if (!sao)
				continue;

			// Do not send to distant clients
			if (sao->getBasePosition().getDistanceFromSQ(pos) > radius_sq)
				continue;

			SendSpawnParticle(maskedu16(p->player_id), p->protocol_version, pp);
		}
		return;
	}
	assert(protocol_version != 0);

	u16 SrvID = ServersNetworkObject->QueryThisServerID();

	NetworkPacket pkt(TOCLIENT_SPAWN_PARTICLE, 2 + 1, 0, protocol_version);
	pkt << (u8)SrvID;
	pkt << playerid.value;
	{
		// NetworkPacket and iostreams are incompatible... OK
		std::ostringstream oss(std::ios_base::binary);
		pp.serialize(oss, protocol_version);
		pkt.putRawString(oss.str());
	}
	Send(&pkt);
}


// Adds a ParticleSpawner on peer with peer_id
void Server::SendAddParticleSpawner(session_t peer_id, u16 protocol_version, const ParticleSpawnerParameters &p, u16 attached_id, u32 id) {
	static thread_local const float radius = g_settings->getS16("max_block_send_distance") * MAP_BLOCKSIZE * BS;

	if (peer_id == PEER_ID_INEXISTENT) {
		std::vector<session_t> clients = m_clients.getClientIDs();
		const v3f pos = (p.minpos + p.maxpos) / 2.0f * BS;
		const float radius_sq = radius * radius;
		/* Don't send short-lived spawners to distant players.
		 * This could be replaced with proper tracking at some point. */
		const bool distance_check = !attached_id && p.time <= 1.0f;

		for (const session_t client_id : clients) {
			RemotePlayer *player = m_env->getPlayer(client_id);
			if (!player)
				continue;

			if (distance_check) {
				PlayerSAO *sao = player->getPlayerSAO();
				if (!sao)
					continue;
				if (sao->getBasePosition().getDistanceFromSQ(pos) > radius_sq)
					continue;
			}

			SendAddParticleSpawner(client_id, player->protocol_version,
				p, attached_id, id);
		}
		return;
	}
	assert(protocol_version != 0);

	NetworkPacket pkt(TOCLIENT_ADD_PARTICLESPAWNER, 100, peer_id, protocol_version);

	pkt << p.amount << p.time << p.minpos << p.maxpos << p.minvel << p.maxvel << p.minacc << p.maxacc << p.minexptime << p.maxexptime << p.minsize << p.maxsize << p.collisiondetection;

	pkt.putLongString(p.texture);

	pkt << id << p.vertical << p.collision_removal << attached_id;
	{
		std::ostringstream os(std::ios_base::binary);
		p.animation.serialize(os, protocol_version);
		pkt.putRawString(os.str());
	}
	pkt << p.glow << p.object_collision;
	pkt << p.node.param0 << p.node.param2 << p.node_tile;

	Send(&pkt);
}

// Adds a ParticleSpawner on peer with peer_id
void Server::SendAddParticleSpawner(maskedu16 playerid, u16 protocol_version, const ParticleSpawnerParameters &p, u16 attached_id, u32 id) {
	static thread_local const float radius = g_settings->getS16("max_block_send_distance") * MAP_BLOCKSIZE * BS;
	if (playerid.value == (u16)0) {
		std::vector<RemotePlayer *> clients = m_env->getPlayers();
		const v3f pos = (p.minpos + p.maxpos) / 2.0f * BS;
		const float radius_sq = radius * radius;
		/* Don't send short-lived spawners to distant players.
		 * This could be replaced with proper tracking at some point. */
		const bool distance_check = !attached_id && p.time <= 1.0f;

		for (RemotePlayer *p_ : clients) {
			if (!p_)
				continue;

			if (distance_check) {
				PlayerSAO *sao = p_->getPlayerSAO();
				if (!sao)
					continue;
				if (sao->getBasePosition().getDistanceFromSQ(pos) > radius_sq)
					continue;
			}

			SendAddParticleSpawner(maskedu16(p_->player_id), p_->protocol_version, p, attached_id, id);
		}
		return;
	}
	
	assert(protocol_version != 0);
	NetworkPacket pkt(TOCLIENT_ADD_PARTICLESPAWNER, 100 + 2 + 1, 0, protocol_version);
	u16 SrvID = ServersNetworkObject->QueryThisServerID();
	pkt << (u8)SrvID;
	pkt << playerid.value;
	pkt << p.amount << p.time << p.minpos << p.maxpos << p.minvel << p.maxvel << p.minacc << p.maxacc << p.minexptime << p.maxexptime << p.minsize << p.maxsize << p.collisiondetection;
	pkt.putLongString(p.texture);
	pkt << id << p.vertical << p.collision_removal << attached_id;
	{
		std::ostringstream os(std::ios_base::binary);
		p.animation.serialize(os, protocol_version);
		pkt.putRawString(os.str());
	}
	pkt << p.glow << p.object_collision;
	pkt << p.node.param0 << p.node.param2 << p.node_tile;
	Send(&pkt);
}


void Server::SendDeleteParticleSpawner(session_t peer_id, u32 id)
{
	NetworkPacket pkt(TOCLIENT_DELETE_PARTICLESPAWNER, 4, peer_id);

	pkt << id;

	if (peer_id != PEER_ID_INEXISTENT)
		Send(&pkt);
	else
		m_clients.sendToAll(&pkt);

}

//variation for SendDeleteParticleSpawner uint16_t
void Server::SendDeleteParticleSpawner(maskedu16 playerid, u32 id)
{
	if (playerid.value != 0) {
		NetworkPacket pkt(TOCLIENT_DELETE_PARTICLESPAWNER, 6);
		pkt << playerid.value;
		pkt << id;
		Send(&pkt);
	} else {
		//Send to all clients [slave->proxy->player] the packet
		NetworkPacket pkt(0x63, 7); // s32 + u16 + u16: 4 + 2 + 1 = 7
		//Get this slave ID
		u16 SrvID = ServersNetworkObject->QueryThisServerID();
		std::vector<RemotePlayer *> players = m_env->getPlayers();
		for (RemotePlayer *p : players) {
			u16 pid = p->getPlayerID();
			pkt << (u8)SrvID;
			pkt << pid;
			pkt << id;
			Send(&pkt);
		}
	}

}

void Server::SendHUDAdd(maskedu16 pid, u32 id, HudElement *form)
{
	NetworkPacket pkt(TOCLIENT_HUDADD, 0);

	pkt << (u8) ServersNetworkObject->QueryThisServerID() << pid.value;

	pkt << id << (u8) form->type << form->pos << form->name << form->scale << form->text << form->number << form->item << form->dir << form->align << form->offset << form->world_pos << form->size << form->z_index << form->text2;

	Send(&pkt);
}

void Server::SendHUDAdd(session_t peer_id, u32 id, HudElement *form)
{
	NetworkPacket pkt(TOCLIENT_HUDADD, 0 , peer_id, m_clients.getProtocolVersion(peer_id));

	pkt << id << (u8) form->type << form->pos << form->name << form->scale
			<< form->text << form->number << form->item << form->dir
			<< form->align << form->offset << form->world_pos << form->size
			<< form->z_index << form->text2;

	Send(&pkt);
}

void Server::SendHUDRemove(session_t peer_id, u32 id)
{
	NetworkPacket pkt(TOCLIENT_HUDRM, 4, peer_id);
	pkt << id;
	Send(&pkt);
}

void Server::SendHUDRemove(maskedu16 pid, u32 id)
{
	NetworkPacket pkt(TOCLIENT_HUDRM, 4);
	pkt << (u8) ServersNetworkObject->QueryThisServerID() << pid.value;
	pkt << id;
	Send(&pkt);
}


void Server::SendHUDChange(maskedu16 pid, u32 id, HudElementStat stat, void *value)
{
	NetworkPacket pkt(TOCLIENT_HUDCHANGE, 0);
	
	pkt << (u8) ServersNetworkObject->QueryThisServerID() << pid.value;
	
	pkt << id << (u8) stat;

	switch (stat) {
		case HUD_STAT_POS:
		case HUD_STAT_SCALE:
		case HUD_STAT_ALIGN:
		case HUD_STAT_OFFSET:
			pkt << *(v2f *) value;
			break;
		case HUD_STAT_NAME:
		case HUD_STAT_TEXT:
		case HUD_STAT_TEXT2:
			pkt << *(std::string *) value;
			break;
		case HUD_STAT_WORLD_POS:
			pkt << *(v3f *) value;
			break;
		case HUD_STAT_SIZE:
			pkt << *(v2s32 *) value;
			break;
		case HUD_STAT_NUMBER:
		case HUD_STAT_ITEM:
		case HUD_STAT_DIR:
		default:
			pkt << *(u32 *) value;
			break;
	}

	Send(&pkt);
}

void Server::SendHUDChange(session_t peer_id, u32 id, HudElementStat stat, void *value)
{
	NetworkPacket pkt(TOCLIENT_HUDCHANGE, 0, peer_id, m_clients.getProtocolVersion(peer_id));
	pkt << id << (u8) stat;

	switch (stat) {
		case HUD_STAT_POS:
		case HUD_STAT_SCALE:
		case HUD_STAT_ALIGN:
		case HUD_STAT_OFFSET:
			pkt << *(v2f *) value;
			break;
		case HUD_STAT_NAME:
		case HUD_STAT_TEXT:
		case HUD_STAT_TEXT2:
			pkt << *(std::string *) value;
			break;
		case HUD_STAT_WORLD_POS:
			pkt << *(v3f *) value;
			break;
		case HUD_STAT_SIZE:
			pkt << *(v2s32 *) value;
			break;
		case HUD_STAT_NUMBER:
		case HUD_STAT_ITEM:
		case HUD_STAT_DIR:
		default:
			pkt << *(u32 *) value;
			break;
	}

	Send(&pkt);
}

void Server::SendHUDSetFlags(session_t peer_id, u32 flags, u32 mask)
{
	NetworkPacket pkt(TOCLIENT_HUD_SET_FLAGS, 4 + 4, peer_id);

	flags &= ~(HUD_FLAG_HEALTHBAR_VISIBLE | HUD_FLAG_BREATHBAR_VISIBLE);

	pkt << flags << mask;

	Send(&pkt);
}

void Server::SendHUDSetFlags(maskedu16 pid, u32 flags, u32 mask)
{
	NetworkPacket pkt(TOCLIENT_HUD_SET_FLAGS, 4 + 4);

	pkt << (u8) ServersNetworkObject->QueryThisServerID() << pid.value;

	flags &= ~(HUD_FLAG_HEALTHBAR_VISIBLE | HUD_FLAG_BREATHBAR_VISIBLE);

	pkt << flags << mask;

	Send(&pkt);
}

void Server::SendHUDSetParam(session_t peer_id, u16 param, const std::string &value)
{
	NetworkPacket pkt(TOCLIENT_HUD_SET_PARAM, 0, peer_id);
	pkt << param << value;
	Send(&pkt);
}

void Server::SendHUDSetParam(maskedu16 pid, u16 param, const std::string &value)
{
	NetworkPacket pkt(TOCLIENT_HUD_SET_PARAM, 0);
	pkt << (u8) ServersNetworkObject->QueryThisServerID() << pid.value;
	pkt << param << value;
	Send(&pkt);
}

void Server::SendSetSky(session_t peer_id, const SkyboxParams &params)
{
	NetworkPacket pkt(TOCLIENT_SET_SKY, 0, peer_id);

	// Handle prior clients here
	if (m_clients.getProtocolVersion(peer_id) < 39) {
		pkt << params.bgcolor << params.type << (u16) params.textures.size();

		for (const std::string& texture : params.textures)
			pkt << texture;

		pkt << params.clouds;
	} else { // Handle current clients and future clients
		pkt << params.bgcolor << params.type
		<< params.clouds << params.fog_sun_tint
		<< params.fog_moon_tint << params.fog_tint_type;

		if (params.type == "skybox") {
			pkt << (u16) params.textures.size();
			for (const std::string &texture : params.textures)
				pkt << texture;
		} else if (params.type == "regular") {
			pkt << params.sky_color.day_sky << params.sky_color.day_horizon
				<< params.sky_color.dawn_sky << params.sky_color.dawn_horizon
				<< params.sky_color.night_sky << params.sky_color.night_horizon
				<< params.sky_color.indoors;
		}
	}

	Send(&pkt);
}

void Server::SendSetSky(maskedu16 pid, const SkyboxParams &params) {
	NetworkPacket pkt(TOCLIENT_SET_SKY, 0);

	pkt << (u8) ServersNetworkObject->QueryThisServerID();
	pkt << pid.value;

	RemotePlayer *player = m_env->getPlayer(pid);

	// Handle prior clients here
	if (player->protocol_version < 39) {
		pkt << params.bgcolor << params.type << (u16) params.textures.size();

		for (const std::string& texture : params.textures)
			pkt << texture;

		pkt << params.clouds;
	} else { // Handle current clients and future clients
		pkt << params.bgcolor << params.type
		<< params.clouds << params.fog_sun_tint
		<< params.fog_moon_tint << params.fog_tint_type;

		if (params.type == "skybox") {
			pkt << (u16) params.textures.size();
			for (const std::string &texture : params.textures)
				pkt << texture;
		} else if (params.type == "regular") {
			pkt << params.sky_color.day_sky << params.sky_color.day_horizon
				<< params.sky_color.dawn_sky << params.sky_color.dawn_horizon
				<< params.sky_color.night_sky << params.sky_color.night_horizon
				<< params.sky_color.indoors;
		}
	}

	Send(&pkt);
}

void Server::SendSetSun(session_t peer_id, const SunParams &params)
{
	NetworkPacket pkt(TOCLIENT_SET_SUN, 0, peer_id);
	pkt << params.visible << params.texture
		<< params.tonemap << params.sunrise
		<< params.sunrise_visible << params.scale;

	Send(&pkt);
}

void Server::SendSetSun(maskedu16 pid, const SunParams &params)
{
	NetworkPacket pkt(TOCLIENT_SET_SUN, 0);
	
	pkt << (u8) ServersNetworkObject->QueryThisServerID() << pid.value;
	
	pkt << params.visible << params.texture
		<< params.tonemap << params.sunrise
		<< params.sunrise_visible << params.scale;

	Send(&pkt);
}

void Server::SendSetMoon(session_t peer_id, const MoonParams &params)
{
	NetworkPacket pkt(TOCLIENT_SET_MOON, 0, peer_id);

	pkt << params.visible << params.texture
		<< params.tonemap << params.scale;

	Send(&pkt);
}

void Server::SendSetMoon(maskedu16 pid, const MoonParams &params)
{
	NetworkPacket pkt(TOCLIENT_SET_MOON, 0);

	pkt << (u8) ServersNetworkObject->QueryThisServerID() << pid.value;

	pkt << params.visible << params.texture << params.tonemap << params.scale;

	Send(&pkt);
}

void Server::SendSetStars(session_t peer_id, const StarParams &params)
{
	NetworkPacket pkt(TOCLIENT_SET_STARS, 0, peer_id);

	pkt << params.visible << params.count
		<< params.starcolor << params.scale;

	Send(&pkt);
}

void Server::SendSetStars(maskedu16 pid, const StarParams &params)
{
	NetworkPacket pkt(TOCLIENT_SET_STARS, 0);

	pkt << (u8) ServersNetworkObject->QueryThisServerID() << pid.value;

	pkt << params.visible << params.count
		<< params.starcolor << params.scale;

	Send(&pkt);
}

void Server::SendCloudParams(session_t peer_id, const CloudParams &params)
{
	NetworkPacket pkt(TOCLIENT_CLOUD_PARAMS, 0, peer_id, m_clients.getProtocolVersion(peer_id));
	pkt << params.density << params.color_bright << params.color_ambient
			<< params.height << params.thickness << params.speed;
	Send(&pkt);
}

void Server::SendCloudParams(maskedu16 pid, const CloudParams &params)
{
	NetworkPacket pkt(TOCLIENT_CLOUD_PARAMS, 0);
	pkt << (u8) ServersNetworkObject->QueryThisServerID() << pid.value;
	pkt << params.density << params.color_bright << params.color_ambient << params.height << params.thickness << params.speed;
	Send(&pkt);
}

void Server::SendOverrideDayNightRatio(session_t peer_id, bool do_override,
		float ratio)
{
	NetworkPacket pkt(TOCLIENT_OVERRIDE_DAY_NIGHT_RATIO, 1 + 2, peer_id);

	pkt << do_override << (u16) (ratio * 65535);

	Send(&pkt);
}

void Server::SendOverrideDayNightRatio(maskedu16 pid, bool do_override, float ratio) {
	NetworkPacket pkt(TOCLIENT_OVERRIDE_DAY_NIGHT_RATIO, 1 + 2);

	pkt << (u8) ServersNetworkObject->QueryThisServerID() << pid.value;

	pkt << do_override << (u16) (ratio * 65535);

	Send(&pkt);
}

void Server::SendTimeOfDay(session_t peer_id, u16 time, f32 time_speed)
{
	if (!ServersNetworkObject->AreSlave) {
		//Only used to send time of day to a player
		if (peer_id != PEER_ID_INEXISTENT) {
			NetworkPacket pkt(TOCLIENT_TIME_OF_DAY, 0, peer_id,
				m_clients.getProtocolVersion(peer_id));
			pkt << time << time_speed;

			Send(&pkt);
			return;
		}

		//this executes to update the world
		NetworkPacket pkt(TOCLIENT_TIME_OF_DAY, 0, peer_id, 37);
		NetworkPacket legacypkt(TOCLIENT_TIME_OF_DAY, 0, peer_id, 32);
		pkt << time << time_speed;
		legacypkt << time << time_speed; //HGZ
		//m_clients.sendToAllCompat(&pkt, &legacypkt, 37);
		m_clients.lock();
		const RemoteClientMap &clients = m_clients.getClientList();
		for (const auto &client_it : clients) {
			RemoteClient *client = client_it.second;
			
			RemotePlayer *player = m_env->getPlayer(client->peer_id);
			
			if (!player)
				continue;
			
			if (player->OnServer->IsApplied)
				continue;
			
			if (client->net_proto_version >= 37) {
				pkt.setPeerID(client->peer_id);
				Send(&pkt);
			} else if (client->net_proto_version != 0) {
				legacypkt.setPeerID(client->peer_id);
				Send(&pkt);
			}
		}
		m_clients.unlock();
	} else {
		//peer_id is PEER_ID_INEXISTENT in this case
		NetworkPacket pkt(TOCLIENT_TIME_OF_DAY, 0, 0, 37);
		pkt << (u8)ServersNetworkObject->QueryThisServerID() << time << time_speed;
		Send(&pkt);
	}
}

void Server::SendPlayerHP(session_t peer_id)
{
	PlayerSAO *playersao = getPlayerSAO(peer_id);
	assert(playersao);

	SendHP(peer_id, playersao->getHP());
	m_script->player_event(playersao,"health_changed");

	// Send to other clients
	playersao->sendPunchCommand();
}

void Server::SendPlayerHP(maskedu16 pid)
{
	PlayerSAO *playersao = getPlayerSAO(pid);
	assert(playersao);
	SendHP(pid, playersao->getHP());
	m_script->player_event(playersao, "health_changed");
	playersao->sendPunchCommand();
}

void Server::SendPlayerBreath(PlayerSAO *sao)
{
	assert(sao);

	m_script->player_event(sao, "breath_changed");
	if (!ServersNetworkObject->AreSlave)
		SendBreath(sao->getPeerID(), sao->getBreath());
	else {
		maskedu16 n(sao->getPlayerID());
		SendBreath(n, sao->getBreath());
	}
}

void Server::SendDisconnectToPlayer(u16 ID) {
	NetworkPacket pkt(0x65, 0);
	pkt << (u8)ServersNetworkObject->QueryThisServerID();
	pkt << ID << (u8) 0 << (u8) 0;
	Send(&pkt);
}

void Server::SendMovePlayer(session_t peer_id)
{
	RemotePlayer *player = m_env->getPlayer(peer_id);
	assert(player);
	PlayerSAO *sao = player->getPlayerSAO();
	assert(sao);

	// Send attachment updates instantly to the client prior updating position
	sao->sendOutdatedData();

	NetworkPacket pkt(TOCLIENT_MOVE_PLAYER, sizeof(v3f) + sizeof(f32) * 2, peer_id, player->protocol_version);
	pkt << sao->getBasePosition() << sao->getLookPitch() << sao->getRotation().Y;

	{
		v3f pos = sao->getBasePosition();
		verbosestream << "Server: Sending TOCLIENT_MOVE_PLAYER"
				<< " pos=(" << pos.X << "," << pos.Y << "," << pos.Z << ")"
				<< " pitch=" << sao->getLookPitch()
				<< " yaw=" << sao->getRotation().Y
				<< std::endl;
	}

	Send(&pkt);
}

void Server::SendMovePlayer(maskedu16 PID)
{
	RemotePlayer *player = m_env->getPlayer(PID);
	assert(player);
	PlayerSAO *sao = player->getPlayerSAO();
	assert(sao);
	// Send attachment updates instantly to the client prior updating position
	sao->sendOutdatedData();
	NetworkPacket pkt(TOCLIENT_MOVE_PLAYER, 1 + 2 + sizeof(v3f) + sizeof(f32) * 2, 0, player->protocol_version);
	pkt << (u8)ServersNetworkObject->QueryThisServerID() << PID.value;
	pkt << sao->getBasePosition() << sao->getLookPitch() << sao->getRotation().Y;
	{
		v3f pos = sao->getBasePosition();
		verbosestream << "Server: Sending TOCLIENT_MOVE_PLAYER" << " pos=(" << pos.X << "," << pos.Y << "," << pos.Z << ")" << " pitch=" << sao->getLookPitch() << " yaw=" << sao->getRotation().Y << std::endl;
	}
	Send(&pkt);
}

void Server::SendPlayerFov(session_t peer_id)
{
	NetworkPacket pkt(TOCLIENT_FOV, 4 + 1 + 4, peer_id);

	PlayerFovSpec fov_spec = m_env->getPlayer(peer_id)->getFov();
	pkt << fov_spec.fov << fov_spec.is_multiplier << fov_spec.transition_time;

	Send(&pkt);
}

void Server::SendPlayerFov(maskedu16 PID)
{
	NetworkPacket pkt(TOCLIENT_FOV, 1 + 2 + 4 + 1 + 4);
	PlayerFovSpec fov_spec = m_env->getPlayer(PID)->getFov();
	
	pkt << uint8_t(ServersNetworkObject->QueryThisServerID()) << PID.value;
	
	pkt << fov_spec.fov << fov_spec.is_multiplier << fov_spec.transition_time;

	Send(&pkt);
}

void Server::SendLocalPlayerAnimations(session_t peer_id, v2s32 animation_frames[4],
		f32 animation_speed)
{
	NetworkPacket pkt(TOCLIENT_LOCAL_PLAYER_ANIMATIONS, 0, peer_id, m_clients.getProtocolVersion(peer_id));

	pkt << animation_frames[0] << animation_frames[1] << animation_frames[2]
			<< animation_frames[3] << animation_speed;

	Send(&pkt);
}

void Server::SendLocalPlayerAnimations(maskedu16 player, v2s32 animation_frames[4], f32 animation_speed)
{
	NetworkPacket pkt(TOCLIENT_LOCAL_PLAYER_ANIMATIONS, 0);

	pkt << (u8) ServersNetworkObject->QueryThisServerID() << player.value; //send data

	pkt << animation_frames[0] << animation_frames[1] << animation_frames[2] << animation_frames[3] << animation_speed;

	Send(&pkt);
}

void Server::SendEyeOffset(session_t peer_id, v3f first, v3f third)
{
	NetworkPacket pkt(TOCLIENT_EYE_OFFSET, 0, peer_id, m_clients.getProtocolVersion(peer_id));
	pkt << first << third;
	Send(&pkt);
}

void Server::SendEyeOffset(maskedu16 pid, v3f first, v3f third) {
	NetworkPacket pkt(TOCLIENT_EYE_OFFSET, 0, ClientDataTable[pid.value]->net_proto_version);
	pkt << (u8) ServersNetworkObject->QueryThisServerID();
	pkt << pid.value;
	pkt << first << third;
	Send(&pkt);
}

void Server::SendPlayerPrivileges(session_t peer_id)
{
	RemotePlayer *player = m_env->getPlayer(peer_id);
	assert(player);
	if(player->getPeerId() == PEER_ID_INEXISTENT)
		return;

	std::set<std::string> privs;
	m_script->getAuth(player->getName(), NULL, &privs);

	NetworkPacket pkt(TOCLIENT_PRIVILEGES, 0, peer_id);
	pkt << (u16) privs.size();

	for (const std::string &priv : privs) {
		pkt << priv;
	}

	Send(&pkt);
}

void Server::SendPlayerPrivileges(maskedu16 pid)
{
	RemotePlayer *player = m_env->getPlayer(pid);
	assert(player);

	std::set<std::string> privs;
	m_script->getAuth(player->getName(), NULL, &privs);

	NetworkPacket pkt(TOCLIENT_PRIVILEGES, 0);
	
	pkt << (u8) ServersNetworkObject->QueryThisServerID();
	pkt << pid.value;
	
	pkt << (u16) privs.size();

	for (const std::string &priv : privs) {
		pkt << priv;
	}

	Send(&pkt);
}

void Server::SendPlayerInventoryFormspec(session_t peer_id)
{
	RemotePlayer *player = m_env->getPlayer(peer_id);
	assert(player);
	if (player->getPeerId() == PEER_ID_INEXISTENT)
		return;

	NetworkPacket pkt(TOCLIENT_INVENTORY_FORMSPEC, 0, peer_id);
	if (player->protocol_version < 37)
		pkt.putLongString(insert_formspec_prepend(player->inventory_formspec,
			player->formspec_prepend));
	else
		pkt.putLongString(player->inventory_formspec);

	Send(&pkt);
}

void Server::SendPlayerInventoryFormspec(maskedu16 pid)
{
	RemotePlayer *player = m_env->getPlayer(pid);
	assert(player);

	NetworkPacket pkt(TOCLIENT_INVENTORY_FORMSPEC, 0);
	
	pkt << (u8) ServersNetworkObject->QueryThisServerID();
	pkt << pid.value;
	
	if (player->protocol_version < 37)
		pkt.putLongString(insert_formspec_prepend(player->inventory_formspec, player->formspec_prepend));
	else
		pkt.putLongString(player->inventory_formspec);
	Send(&pkt);
}

void Server::SendPlayerFormspecPrepend(session_t peer_id)
{
	RemotePlayer *player = m_env->getPlayer(peer_id);
	assert(player);
	if (player->getPeerId() == PEER_ID_INEXISTENT)
		return;
	if (player->protocol_version < 37) {
		SendPlayerInventoryFormspec(peer_id);
		return;
	}

	NetworkPacket pkt(TOCLIENT_FORMSPEC_PREPEND, 0, peer_id);
	pkt << player->formspec_prepend;
	Send(&pkt);
}

void Server::SendPlayerFormspecPrepend(maskedu16 pid)
{
	RemotePlayer *player = m_env->getPlayer(pid);
	assert(player);
	
	if (player->protocol_version < 37) {
		SendPlayerInventoryFormspec(pid);
		return;
	}

	NetworkPacket pkt(TOCLIENT_FORMSPEC_PREPEND, 2 + 1);
	
	pkt << (u8) ServersNetworkObject->QueryThisServerID() << pid.value;
	
	pkt << player->formspec_prepend;
	Send(&pkt);
}

void Server::SendActiveObjectRemoveAdd(RemoteClient *client, PlayerSAO *playersao)
{
	// Radius inside which objects are active
	static thread_local const s16 radius =
		g_settings->getS16("active_object_send_range_blocks") * MAP_BLOCKSIZE;

	// Radius inside which players are active
	static thread_local const bool is_transfer_limited =
		g_settings->exists("unlimited_player_transfer_distance") &&
		!g_settings->getBool("unlimited_player_transfer_distance");

	static thread_local const s16 player_transfer_dist =
		g_settings->getS16("player_transfer_distance") * MAP_BLOCKSIZE;

	s16 player_radius = player_transfer_dist == 0 && is_transfer_limited ?
		radius : player_transfer_dist;

	s16 my_radius = MYMIN(radius, playersao->getWantedRange() * MAP_BLOCKSIZE);
	if (my_radius <= 0)
		my_radius = radius;

	std::queue<u16> removed_objects, added_objects;
	m_env->getRemovedActiveObjects(playersao, my_radius, player_radius,
		client->m_known_objects, removed_objects);
	m_env->getAddedActiveObjects(playersao, my_radius, player_radius,
		client->m_known_objects, added_objects);

	int removed_count = removed_objects.size();
	int added_count   = added_objects.size();

	if (removed_objects.empty() && added_objects.empty())
		return;

	char buf[4];
	std::string data;

	// Handle removed objects
	writeU16((u8*)buf, removed_objects.size());
	data.append(buf, 2);
	while (!removed_objects.empty()) {
		// Get object
		u16 id = removed_objects.front();
		ServerActiveObject* obj = m_env->getActiveObject(id);

		// Add to data buffer for sending
		writeU16((u8*)buf, id);
		data.append(buf, 2);
		// Remove from known objects
		client->m_known_objects.erase(id);

		if (obj && obj->m_known_by_count > 0)
			obj->m_known_by_count--;

		removed_objects.pop();
	}

	// Handle added objects
	writeU16((u8*)buf, added_objects.size());
	data.append(buf, 2);
	while (!added_objects.empty()) {
		// Get object
		u16 id = added_objects.front();
		ServerActiveObject *obj = m_env->getActiveObject(id);
		added_objects.pop();

		if (!obj) {
			warningstream << FUNCTION_NAME << ": NULL object id="
				<< (int)id << std::endl;
			continue;
		}

		// Get object type
		u8 type = obj->getSendType();

		// Add to data buffer for sending
		writeU16((u8*)buf, id);
		data.append(buf, 2);
		writeU8((u8*)buf, type);
		data.append(buf, 1);

		data.append(serializeString32(
			obj->getClientInitializationData(client->net_proto_version)));

		// Add to known objects
		client->m_known_objects.insert(id);

		obj->m_known_by_count++;
	}

	//Debug
	//std::cout << std::string(data.c_str(), data.size()) << std::endl;

	NetworkPacket pkt(TOCLIENT_ACTIVE_OBJECT_REMOVE_ADD, data.size(), client->peer_id);
	pkt.putRawString(data.c_str(), data.size());
	Send(&pkt);

	verbosestream << "Server::SendActiveObjectRemoveAdd: "
		<< removed_count << " removed, " << added_count << " added, "
		<< "packet size is " << pkt.getSize() << std::endl;
}

//Should send to remove all objects to make the client only know himself, as going to other server from server
void Server::SendRemoveObjectsToClient(u16 pid) {
	RemotePlayer *p = m_env->getPlayer(maskedu16(pid));
	if (!p) //lost frame
		return;
	RemoteClient *client = m_clients.lockedGetClientNoEx(p->getPeerId());
	PlayerSAO *sao = p->getPlayerSAO();
	u16 ID = sao->getId();
	char buf[4];
	std::string data;
	
	writeU16((u8*)buf, client->m_known_objects.size()-1); //-1 is player
	data.append(buf, 2);
	
	for (std::set<u16>::iterator it = client->m_known_objects.begin(); it != client->m_known_objects.end(); ++it) {
		ServerActiveObject* obj = m_env->getActiveObject(*it);
		if (obj) {
			if (obj->m_known_by_count > 0)
				obj->m_known_by_count--;
			if (obj->getId() == ID) //skip player FIXME!
				continue;
		}
		writeU16((u8*)buf, *it);
		data.append(buf, 2);
	}
	client->m_known_objects.erase(client->m_known_objects.begin(), client->m_known_objects.end());
	//prepare data
	NetworkPacket pkt(TOCLIENT_ACTIVE_OBJECT_REMOVE_ADD, data.size(), client->peer_id);
	pkt.putRawString(data.c_str(), data.size());
	Send(&pkt);
}

void Server::SendActiveObjectRemoveAdd_SLAVE(ClientDataHelper *client, PlayerSAO *playersao)
{
	///if (client->FirstStep) {
	//	client->FirstStep = false;
	//	return;
	//}
	// Radius inside which objects are active
	static thread_local const s16 radius = g_settings->getS16("active_object_send_range_blocks") * MAP_BLOCKSIZE;
	// Radius inside which players are active
	static thread_local const bool is_transfer_limited = g_settings->exists("unlimited_player_transfer_distance") && !g_settings->getBool("unlimited_player_transfer_distance");
	static thread_local const s16 player_transfer_dist = g_settings->getS16("player_transfer_distance") * MAP_BLOCKSIZE;

	s16 player_radius = player_transfer_dist == 0 && is_transfer_limited ? radius : player_transfer_dist;

	s16 my_radius = MYMIN(radius, playersao->getWantedRange() * MAP_BLOCKSIZE);
	if (my_radius <= 0)
		my_radius = radius;

	std::queue<u16> removed_objects, added_objects, ro, ao;
	m_env->getRemovedActiveObjects(playersao, my_radius, player_radius, client->m_known_objects, removed_objects);
	m_env->getAddedActiveObjects(playersao, my_radius, player_radius, client->m_known_objects, added_objects);

	int removed_count = removed_objects.size();
	int added_count = added_objects.size();

	if (removed_objects.empty() && added_objects.empty())
		return;

	//MasterServerUniqueIDS *SuperIDs = client->SuperIDs;
	//if (!SuperIDs->hasThisID(aom.id))
		//return;

	char buf[4];
	std::string data;

	size_t buf_size_ao = added_objects.size();
	bool ao_rmv_1 = false;
	size_t bug_size_do = removed_objects.size();
	bool do_rmv_1 = false;

	//this
	/*while (!removed_objects.empty()) {
		u16 id = removed_objects.front();
		ServerActiveObject *obj = m_env->getActiveObject(id);
		client->m_known_objects.erase(id);
		if (obj->getType() == ACTIVEOBJECT_TYPE_PLAYER)
			if (id == m_env->getPlayer(maskedu16(client->GetPlayerID_()))->IdForSao) {
				removed_objects.pop();
				continue;
			}
		ro.push(id);
		removed_objects.pop();
	}*/

	//NOTE: WE WILL NOT USE AGAIN SuperIDs AS IT ARE BEING USED IN PROXY

	// Handle removed objects
	writeU16((u8*)buf, ro.size());
	data.append(buf, 2);
	while (!removed_objects.empty()) {
		// Get object
		u16 id = removed_objects.front();
		ServerActiveObject* obj = m_env->getActiveObject(id);
		if (obj && obj->m_known_by_count > 0)
			obj->m_known_by_count--;
		client->m_known_objects.erase(id);
		// Add to data buffer for sending
		writeU16((u8*)buf, id);//SuperIDs->getUniqueID(id, client->IdInSlave));
		data.append(buf, 2);
		
		//May M.S.U.I know this object?
		//if (SuperIDs->hasThisID(id))
		//	SuperIDs->deleteUniqueID(id);
			
		removed_objects.pop();
	}

	//Should precheck at loading the player mesh, if the own player are present this might make exceptions like this: "Reading outside packet"
	/*while (!added_objects.empty()) {
		u16 id = added_objects.front();
		ServerActiveObject *obj = m_env->getActiveObject(id);
		client->m_known_objects.insert(id);
		obj->m_known_by_count++;
		if (obj->getType() == ACTIVEOBJECT_TYPE_PLAYER)
			if (id == m_env->getPlayer(maskedu16(client->GetPlayerID_()))->IdForSao) {
				added_objects.pop();
				continue;
			}
		ao.push(id);
		added_objects.pop();
	}*/

	// Handle added objects
	// If we will initialize player, send a AOM message
	// Send a direct packet to the main server that we got a player to initialize (During initialization time of the player)
	writeU16((u8*)buf, ao.size());
	data.append(buf, 2);
	while (!added_objects.empty()) {
		// Get object
		u16 id = added_objects.front();
		ServerActiveObject *obj = m_env->getActiveObject(id);
		added_objects.pop();
		client->m_known_objects.insert(id);
		obj->m_known_by_count++;
		if (!obj) {
			warningstream << FUNCTION_NAME << ": NULL object id=" << (int)id << std::endl;
			continue;
		}
		
		if (obj->getType() == ACTIVEOBJECT_TYPE_PLAYER) { //A player is being initialized
			RemotePlayer *player = m_env->FindPlayerWithThisId(obj->getId());
			verbosestream << FUNCTION_NAME << ": Initialization->Player" << std::endl;
			if (!player) {
				warningstream << FUNCTION_NAME << ": Not known player: " << obj->getId() << std::endl;
				continue;
			}

			//Let's make a packet with the initialization data
			NetworkPacket pkt_(0x77, 3); // 0x77: Single Packet AOM redirected to a specific player on the game
			std::string _init = obj->GetInitData(client->net_proto_version);
			verbosestream << FUNCTION_NAME << ": InitStringLen: " << _init.size() << std::endl;
			
			std::string buff_;
			char bf[2];
			writeU8((u8*)bf, (u8)ServersNetworkObject->QueryThisServerID());
			buff_.append(bf, 1);
			writeU16((u8*)bf, player->player_id);
			buff_.append(bf, 2);
			buff_.append(_init);
			
			pkt_.putRawString(buff_.c_str(), buff_.size());
			
			Send(&pkt_);
			//continue;
		}
		
		//Make the MSUI know this obj
		//u16 O_ID = 0;
		//if (!SuperIDs->hasThisID(id))
		//	O_ID = SuperIDs->registerUniqueID(id);
		
		// Get object type
		u8 type = obj->getSendType();
		// Add to data buffer for sending
		writeU16((u8*)buf, id);
		data.append(buf, 2);
		writeU8((u8*)buf, type);
		data.append(buf, 1);
		
		data.append(serializeString32(obj->getClientInitializationData(client->net_proto_version)));
	}

	/* FIXED BY ADDING A NEW ENGINE CLASS 'SlaveServerObjects' */
	//Should return if the data are only 1 obj [A player]
	//If a player was in the initialize data and the initialized data are the player which we will send, don't send, the player has already the A.O.M, or else, SIGSEGV <getParent(), comparision, m_cao>
	//if (buf_size_ao == 1 && ao_rmv_1) {
	//	warningstream << "Buffer was only 1 object <player>; Skipping" << std::endl;
	//	return;
	//}

	//Same as the buf_size_ao
	//if (bug_size_do == 1 && do_rmv_1) {
	//	warningstream << "Buffer was only 1 object <player>" << std::endl;
	//	return;
	//}

	u16 SrvID = ServersNetworkObject->QueryThisServerID();

	NetworkPacket pkt(TOCLIENT_ACTIVE_OBJECT_REMOVE_ADD, data.size());
	
	pkt << (u8)SrvID;
	pkt << client->GetPlayerID_();
	
	pkt.putRawString(data.c_str(), data.size());
	
	Send(&pkt);

	verbosestream << "Server::SendActiveObjectRemoveAdd: " << removed_count << " removed, " << added_count << " added, " << "packet size is " << pkt.getSize() << std::endl;
	
	//Update the proxy
	std::unordered_map<u16, u16> plistnsao;
	//Known objects by player, check by SAO if they are player N store into 'plistnsao'
	if (ClientDataTable.size() == client->known_players_count)
		return;
	
	client->known_players_count = (u16)ClientDataTable.size();
	
	for (auto it = ClientDataTable.begin(); it != ClientDataTable.end(); it++) {
		//Get SAO
		PlayerSAO *obj = getPlayerSAO(maskedu16(it->second->GetPlayerID_()));
		
		//ServerActiveObject *obj = m_env->getActiveObject(obj_id);
		if (obj && obj->getType() == ACTIVEOBJECT_TYPE_PLAYER) {
			//Get player ID
			u16 p_id = obj->getPlayerID();
			plistnsao[p_id] = obj->getId();
		}
	}
	NetworkPacket pkt_(0x69, 0);
	pkt_ << (u8)ServersNetworkObject->QueryThisServerID() << client->GetPlayerID_() << (u16)plistnsao.size();
	for (auto it = plistnsao.begin(); it != plistnsao.end(); it++)
		pkt_ << it->first << it->second; //[PLAYER_ID] = [OBJECT_ID OF SLAVE (SEEN BY THE PLAYER ON PROXY SIDE)]
	
	Send(&pkt);
}

void Server::SendActiveObjectMessages(session_t peer_id, const std::string &datas,
		bool reliable)
{
	NetworkPacket pkt(TOCLIENT_ACTIVE_OBJECT_MESSAGES,
			datas.size(), peer_id);

	pkt.putRawString(datas.c_str(), datas.size());

	m_clients.send(pkt.getPeerId(),
			reliable ? clientCommandFactoryTable[pkt.getCommand()].channel : 1,
			&pkt, reliable);
}

void Server::SendActiveObjectMessages(maskedu16 pid, const std::string &datas, bool reliable) {
	u8 reliable_data = reliable ? (u8)1 : (u8)0;
	NetworkPacket pkt(TOCLIENT_ACTIVE_OBJECT_MESSAGES, datas.size());
	u8 ID = (u8)ServersNetworkObject->QueryThisServerID();
	pkt << ID; //s_id
	pkt << pid.value; //player
	pkt << reliable_data; //checks in proxy
	pkt.putRawString(datas.c_str(), datas.size());
	Send(&pkt);
}

void Server::SendCSMRestrictionFlags(session_t peer_id)
{
	const u16 protocol_version = m_clients.getProtocolVersion(peer_id);
	if (protocol_version < 35 && protocol_version != 0)
		return;

	NetworkPacket pkt(TOCLIENT_CSM_RESTRICTION_FLAGS,
		sizeof(m_csm_restriction_flags) + sizeof(m_csm_restriction_noderange), peer_id);
	pkt << m_csm_restriction_flags << m_csm_restriction_noderange;
	Send(&pkt);
}

void Server::SendPlayerSpeed(session_t peer_id, const v3f &added_vel)
{
	NetworkPacket pkt(TOCLIENT_PLAYER_SPEED, 0, peer_id);
	pkt << added_vel;
	Send(&pkt);
}

void Server::SendPlayerSpeed(maskedu16 PID, const v3f &added_vel)
{
	u16 SrvID = ServersNetworkObject->QueryThisServerID();
	NetworkPacket pkt(TOCLIENT_PLAYER_SPEED, 3); //2 + 1
	pkt << u8(SrvID);
	pkt << PID.value;
	pkt << added_vel;
	Send(&pkt);
}

inline s32 Server::nextSoundId()
{
	s32 ret = m_next_sound_id;
	if (m_next_sound_id == INT32_MAX)
		m_next_sound_id = 0; // signed overflow is undefined
	else
		m_next_sound_id++;
	return ret;
}

s32 Server::playSound(const SimpleSoundSpec &spec, const ServerSoundParams &params, bool ephemeral)
{
	if (!ServersNetworkObject->AreSlave) {
		// Find out initial position of sound
		bool pos_exists = false;
		v3f pos = params.getPos(m_env, &pos_exists);
		// If position is not found while it should be, cancel sound
		if(pos_exists != (params.type != ServerSoundParams::SSP_LOCAL))
			return -1;

		// Filter destination clients
		std::vector<session_t> dst_clients;
		if (!params.to_player.empty()) {
			RemotePlayer *player = m_env->getPlayer(params.to_player.c_str());
			if(!player){
				infostream<<"Server::playSound: Player \""<<params.to_player
						<<"\" not found"<<std::endl;
				return -1;
			}
			if (player->getPeerId() == PEER_ID_INEXISTENT) {
				infostream<<"Server::playSound: Player \""<<params.to_player
						<<"\" not connected"<<std::endl;
				return -1;
			}
			dst_clients.push_back(player->getPeerId());
		} else {
			std::vector<session_t> clients = m_clients.getClientIDs();

			for (const session_t client_id : clients) {
				RemotePlayer *player = m_env->getPlayer(client_id);
				if (!player)
					continue;
				if (!params.exclude_player.empty() &&
						params.exclude_player == player->getName())
					continue;

				PlayerSAO *sao = player->getPlayerSAO();
				if (!sao)
					continue;

				if (pos_exists) {
					if(sao->getBasePosition().getDistanceFrom(pos) >
							params.max_hear_distance)
						continue;
				}
				dst_clients.push_back(client_id);
			}
		}

		if(dst_clients.empty())
			return -1;

		// Create the sound
		s32 id;
		ServerPlayingSound *psound = nullptr;
		if (ephemeral) {
			id = -1; // old clients will still use this, so pick a reserved ID
		} else {
			id = nextSoundId();
			// The sound will exist as a reference in m_playing_sounds
			m_playing_sounds[id] = ServerPlayingSound();
			psound = &m_playing_sounds[id];
			psound->params = params;
			psound->spec = spec;
		}

		float gain = params.gain * spec.gain;
		NetworkPacket pkt(TOCLIENT_PLAY_SOUND, 0);
		NetworkPacket legacypkt(TOCLIENT_PLAY_SOUND, 0, PEER_ID_INEXISTENT, 32);
		pkt << id << spec.name << gain
				<< (u8) params.type << pos << params.object
				<< params.loop << params.fade << params.pitch
				<< ephemeral;
		legacypkt << id << spec.name << gain
				<< (u8) params.type << pos << params.object
				<< params.loop << params.fade;

		bool as_reliable = !ephemeral;
		bool play_sound = gain > 0;

		for (const u16 dst_client : dst_clients) {
			const u16 protocol_version = m_clients.getProtocolVersion(dst_client);
			if (!play_sound && protocol_version < 32)
				continue;
			if (psound)
				psound->clients.insert(dst_client);

			if (protocol_version >= 37)
				m_clients.send(dst_client, 0, &pkt, as_reliable);
			else
				m_clients.send(dst_client, 0, &legacypkt, as_reliable);
		}
		return id;
	} else {
		// Find out initial position of sound
		bool pos_exists = false;
		v3f pos = params.getPos(m_env, &pos_exists);
		// If position is not found while it should be, cancel sound
		if (pos_exists != (params.type != ServerSoundParams::SSP_LOCAL))
			return -1;
			
		std::vector<u16> tosend;
		u16 counttosend = 0;
		
		if (!params.to_player.empty()) {
			RemotePlayer *player = m_env->getPlayer(params.to_player.c_str());
			if (!player) {
				infostream << "Server::playSound: Player \"" << params.to_player << "\" not found" << std::endl;
				return -1;
			}
			if (player->player_id == 0) {
				infostream << "Server::playSound: Player \"" << params.to_player << "\" not connected"<<std::endl;
				return -1;
			}
			tosend.push_back(player->player_id);
			counttosend++;
		} else {
			for (auto it = ClientDataTable.begin(); it != ClientDataTable.end(); ++it) {
				RemotePlayer *player = m_env->getPlayer(maskedu16(it->first));
				if (!player)
					continue;
				if (!params.exclude_player.empty() && params.exclude_player == player->getName())
					continue;
				
				PlayerSAO *sao = player->getPlayerSAO();
				if (!sao)
					continue;
				
				if (pos_exists) {
					if (sao->getBasePosition().getDistanceFrom(pos) > params.max_hear_distance)
						continue;
				}
				tosend.push_back(it->first);
				counttosend++;
			}
		}
		
		if (tosend.empty())
			return -1;
		
		// Create the sound
		s32 id;
		ServerPlayingSound *psound = nullptr;
		if (ephemeral) {
			id = -1; // old clients will still use this, so pick a reserved ID
		} else {
			id = nextSoundId();
			// The sound will exist as a reference in m_playing_sounds
			m_playing_sounds[id] = ServerPlayingSound();
			psound = &m_playing_sounds[id];
			psound->params = params;
			psound->spec = spec;
		}
		
		float gain = params.gain * spec.gain;
		NetworkPacket pkt(TOCLIENT_PLAY_SOUND, 0);
		//Push server id
		pkt << (u8)ServersNetworkObject->QueryThisServerID();
		//make table [u16 pcount, u16 .. playersid ..]
		pkt << counttosend;
		for (u16 clid : tosend) {
			pkt << clid;
			if (psound)
				psound->clients_int16.insert(clid);
		}
		pkt << id << spec.name << gain << (u8) params.type << pos << params.object << params.loop << params.fade << params.pitch << ephemeral;
		Send(&pkt);
		return id;
	}
}
void Server::stopSound(s32 handle)
{
	if (!ServersNetworkObject->AreSlave) {
		// Get sound reference
		std::unordered_map<s32, ServerPlayingSound>::iterator i = m_playing_sounds.find(handle);
		if (i == m_playing_sounds.end())
			return;
		
		ServerPlayingSound &psound = i->second;

		NetworkPacket pkt(TOCLIENT_STOP_SOUND, 4);
		pkt << handle;

		for (std::unordered_set<session_t>::const_iterator si = psound.clients.begin(); si != psound.clients.end(); ++si) {
			// Send as reliable
			m_clients.send(*si, 0, &pkt, true);
		}
		// Remove sound reference
		m_playing_sounds.erase(i);
	} else {
		// Get sound reference
		std::unordered_map<s32, ServerPlayingSound>::iterator i = m_playing_sounds.find(handle);
		if (i == m_playing_sounds.end())
			return;
		
		ServerPlayingSound &psound = i->second;

		//Send to main proxy server to stop the sound handle

		NetworkPacket pkt(TOCLIENT_STOP_SOUND, 4 + 1 + 2);
		pkt << (u8)ServersNetworkObject->QueryThisServerID();
		pkt << handle;
		pkt << (u16)psound.clients_int16.size();

		for (std::unordered_set<u16>::const_iterator si = psound.clients_int16.begin(); si != psound.clients_int16.end(); ++si) {
			// Send as reliable
			//m_clients.send(*si, 0, &pkt, true);
			pkt << *si; //i think it is an u16....
		}
		Send(&pkt);
		// Remove sound reference
		m_playing_sounds.erase(i);
	}
}

void Server::fadeSound(s32 handle, float step, float gain)
{
	if (!ServersNetworkObject->AreSlave) {
		// Get sound reference
		std::unordered_map<s32, ServerPlayingSound>::iterator i = m_playing_sounds.find(handle);
		if (i == m_playing_sounds.end())
			return;

		ServerPlayingSound &psound = i->second;
		psound.params.gain = gain;

		NetworkPacket pkt(TOCLIENT_FADE_SOUND, 4);
		pkt << handle << step << gain;

		// Backwards compability
		bool play_sound = gain > 0;
		ServerPlayingSound compat_psound = psound;
		compat_psound.clients.clear();

		NetworkPacket compat_pkt(TOCLIENT_STOP_SOUND, 4);
		compat_pkt << handle;

		for (std::unordered_set<u16>::iterator it = psound.clients.begin();
				it != psound.clients.end();) {
			if (m_clients.getProtocolVersion(*it) >= 32) {
				// Send as reliable
				m_clients.send(*it, 0, &pkt, true);
				++it;
			} else {
				compat_psound.clients.insert(*it);
				// Stop old sound
				m_clients.send(*it, 0, &compat_pkt, true);
				psound.clients.erase(it++);
			}
		}

		// Remove sound reference
		if (!play_sound || psound.clients.empty())
			m_playing_sounds.erase(i);

		if (play_sound && !compat_psound.clients.empty()) {
			// Play new sound volume on older clients
			playSound(compat_psound.spec, compat_psound.params);
		}
	} else {
		std::unordered_map<s32, ServerPlayingSound>::iterator i = m_playing_sounds.find(handle);
		if (i == m_playing_sounds.end())
			return;
		ServerPlayingSound &psound = i->second;
		psound.params.gain = gain;
		
		NetworkPacket pkt(TOCLIENT_FADE_SOUND, 4);
		
		pkt << (u8) ServersNetworkObject->QueryThisServerID();
		
		
		u16 Size = (u16)psound.clients_int16.size();
		pkt << Size;
		for (std::unordered_set<u16>::iterator it = psound.clients_int16.begin(); it != psound.clients_int16.end();) {
			pkt << (u16)*it;
		}
		
		pkt << handle << step << gain;
	}
}

void Server::sendRemoveNode(v3s16 p, std::unordered_set<u16> *far_players,
		float far_d_nodes)
{
	float maxd = far_d_nodes * BS;
	v3f p_f = intToFloat(p, BS);
	v3s16 block_pos = getNodeBlockPos(p);
	if (!ServersNetworkObject->AreSlave) {
		NetworkPacket pkt(TOCLIENT_REMOVENODE, 6);
		pkt << p;

		std::vector<session_t> clients = m_clients.getClientIDs();
		m_clients.lock();

		for (session_t client_id : clients) {
			RemoteClient *client = m_clients.lockedGetClientNoEx(client_id);
			if (!client)
				continue;

			RemotePlayer *player = m_env->getPlayer(client_id);
			if (player->OnServer->IsApplied)
				continue;
			PlayerSAO *sao = player ? player->getPlayerSAO() : nullptr;

			// If player is far away, only set modified blocks not sent
			if (!client->isBlockSent(block_pos) || (sao &&
					sao->getBasePosition().getDistanceFrom(p_f) > maxd)) {
				if (far_players)
					far_players->emplace(client_id);
				else
					client->SetBlockNotSent(block_pos);
				continue;
			}

			// Send as reliable
			m_clients.send(client_id, 0, &pkt, true);
		}

		m_clients.unlock();
	} else {
		NetworkPacket pkt(0x67, 0);
		pkt << p;
		std::vector<u16> playersid_tosend;
		for (auto it = ClientDataTable.begin(); it != ClientDataTable.end(); ++it) {
			RemotePlayer *player = m_env->getPlayer(it->second->GetPlayerID_());
			PlayerSAO *sao = player ? player->getPlayerSAO() : nullptr;
			// If player is far away, only set modified blocks not sent
			if (!it->second->isBlockSent(block_pos) || (sao &&
					sao->getBasePosition().getDistanceFrom(p_f) > maxd)) {
				if (far_players)
					far_players->emplace(it->first);
				else
					it->second->SetBlockNotSent(block_pos);
				continue;
			}
			playersid_tosend.push_back(it->first);
		}
		
		u16 size_ = (u16)playersid_tosend.size();
		pkt << size_;
		for (u16 p : playersid_tosend) {
			pkt << p;
		}
		Send(&pkt);
	}
}

void Server::sendAddNode(v3s16 p, MapNode n, std::unordered_set<u16> *far_players, float far_d_nodes, bool remove_metadata)
{
	float maxd = far_d_nodes * BS;
	v3f p_f = intToFloat(p, BS);
	v3s16 block_pos = getNodeBlockPos(p);

	if (!ServersNetworkObject->AreSlave) {
		NetworkPacket pkt(TOCLIENT_ADDNODE, 6 + 2 + 1 + 1 + 1);
		
		pkt << p << n.param0 << n.param1 << n.param2 << (u8) (remove_metadata ? 0 : 1);
	
		std::vector<session_t> clients = m_clients.getClientIDs();
		m_clients.lock();

		for (session_t client_id : clients) {
			RemoteClient *client = m_clients.lockedGetClientNoEx(client_id);
			if (!client)
				continue;

			RemotePlayer *player = m_env->getPlayer(client_id);
			
			if (player->OnServer->IsApplied)
				continue;
			
			PlayerSAO *sao = player ? player->getPlayerSAO() : nullptr;

			// If player is far away, only set modified blocks not sent
			if (!client->isBlockSent(block_pos) || (sao &&
					sao->getBasePosition().getDistanceFrom(p_f) > maxd)) {
				if (far_players)
					far_players->emplace(client_id);
				else
					client->SetBlockNotSent(block_pos);
				continue;
			}

			// Send as reliable
			m_clients.send(client_id, 0, &pkt, true);
		}

		m_clients.unlock();
	} else {
		NetworkPacket pkt(0x83, 6 + 2 + 2 + 1 + 1 + 1 + 1);
		
		u8 SrvID = (u8)ServersNetworkObject->QueryThisServerID();
		
		pkt << SrvID;
		
		std::vector<u16> playersid_tosend;
		
		//make like a simple table just to send 1 packet with all players data to don't send 20+ packets to 1 server or it will be overloaded
		// [0: u16] PLAYERS COUNT [0-INF] PLAYERS [INF-**] DATA
		
		//pkt << p << n.param0 << n.param1 << n.param2 << (u8) (remove_metadata ? 0 : 1);
		for (auto it = ClientDataTable.begin(); it != ClientDataTable.end(); ++it) {
			ClientDataHelper *client = it->second;
			RemotePlayer *player = m_env->getPlayer(it->first);
			PlayerSAO *sao = player ? player->getPlayerSAO() : nullptr;
			if (!client->isBlockSent(block_pos) || (sao && sao->getBasePosition().getDistanceFrom(p_f) > maxd)) {
				
				if (far_players)
					far_players->emplace(client->GetPlayerID_());
				else
					client->SetBlockNotSent(block_pos);
				
				continue;
			}
			
			//pkt << SrvID << it->first << p << n.param0 << n.param1 << n.param2 << (u8) (remove_metadata ? 0 : 1);
			playersid_tosend.push_back(it->first);
		}
		//iterate
		u16 playercount = (u16)playersid_tosend.size();
		pkt << playercount;
		for (u16 pid : playersid_tosend) {
			pkt << pid;
		}
		pkt << p << n.param0 << n.param1 << n.param2 << (u8) (remove_metadata ? 0 : 1);
		Send(&pkt);
	}
}

void Server::sendMetadataChanged(const std::list<v3s16> &meta_updates, float far_d_nodes)
{
	float maxd = far_d_nodes * BS;
	NodeMetadataList meta_updates_list(false);

	if (!ServersNetworkObject->AreSlave) {
		std::vector<session_t> clients = m_clients.getClientIDs();
		m_clients.lock();

		for (session_t i : clients) {
			RemoteClient *client = m_clients.lockedGetClientNoEx(i);
			if (!client)
				continue;

			if (client->net_proto_version < 37) {
				for (const v3s16 &pos : meta_updates) {
					client->SetBlockNotSent(getNodeBlockPos(pos));
				}
				continue;
			}

			ServerActiveObject *player = m_env->getActiveObject(i);
			v3f player_pos = player ? player->getBasePosition() : v3f();

			for (const v3s16 &pos : meta_updates) {
				NodeMetadata *meta = m_env->getMap().getNodeMetadata(pos);

				if (!meta)
					continue;

				v3s16 block_pos = getNodeBlockPos(pos);
				if (!client->isBlockSent(block_pos) || (player &&
						player_pos.getDistanceFrom(intToFloat(pos, BS)) > maxd)) {
					client->SetBlockNotSent(block_pos);
					continue;
				}

				// Add the change to send list
				meta_updates_list.set(pos, meta);
			}
			if (meta_updates_list.size() == 0)
				continue;

			// Send the meta changes
			std::ostringstream os(std::ios::binary);
			meta_updates_list.serialize(os, client->net_proto_version, false, true);
			std::ostringstream oss(std::ios::binary);
			compressZlib(os.str(), oss);

			NetworkPacket pkt(TOCLIENT_NODEMETA_CHANGED, 0);
			pkt.putLongString(oss.str());
			m_clients.send(i, 0, &pkt, true);

			meta_updates_list.clear();
		}

		m_clients.unlock();
	} else {
		//for (u16 val : ClientDataTable) {
		for (auto it = ClientDataTable.begin(); it != ClientDataTable.end(); ++it) {
			if (ClientDataTable.find(it->first) == ClientDataTable.end())
				continue;
			
			ClientDataHelper *client = ClientDataTable[it->first];
			

			RemotePlayer *p = m_env->getPlayer(maskedu16(it->first));

			if (it->second->net_proto_version < 37) {
				for (const v3s16 &pos : meta_updates) {
					it->second->SetBlockNotSent(getNodeBlockPos(pos));
				}
				continue;
			}

			ServerActiveObject *player = m_env->getActiveObject(p->IdForSao);
			v3f player_pos = player ? player->getBasePosition() : v3f();

			for (const v3s16 &pos : meta_updates) {
				NodeMetadata *meta = m_env->getMap().getNodeMetadata(pos);

				if (!meta)
					continue;

				v3s16 block_pos = getNodeBlockPos(pos);
				if (!client->isBlockSent(block_pos) || (player && player_pos.getDistanceFrom(intToFloat(pos, BS)) > maxd)) {
					client->SetBlockNotSent(block_pos);
					continue;
				}

				// Add the change to send list
				meta_updates_list.set(pos, meta);
			}
			if (meta_updates_list.size() == 0)
				continue;

			// Send the meta changes
			std::ostringstream os(std::ios::binary);
			meta_updates_list.serialize(os, client->net_proto_version, false, true);
			std::ostringstream oss(std::ios::binary);
			compressZlib(os.str(), oss);

			NetworkPacket pkt(TOCLIENT_NODEMETA_CHANGED, 0);
			
			pkt << (u8) ServersNetworkObject->QueryThisServerID();
			
			pkt.putLongString(oss.str());
			//m_clients.send(i, 0, &pkt, true);
			Send(&pkt);

			meta_updates_list.clear();
		}
	}
}

void Server::SendBlockNoLock(session_t peer_id, MapBlock *block, u8 ver, u16 net_proto_version)
{
	/*
		Create a packet with the block in the right format
	*/
	thread_local const int net_compression_level = rangelim(g_settings->getS16("map_compression_level_net"), -1, 9);
	std::ostringstream os(std::ios_base::binary);

	RemotePlayer *player = m_env->getPlayer(peer_id);
	if (player && player->protocol_version < 37)
		block->serialize(os, ver, false, net_compression_level,
			player->formspec_prepend);
	else
		block->serialize(os, ver, false, net_compression_level);
	block->serializeNetworkSpecific(os);
	std::string s = os.str();

	NetworkPacket pkt(TOCLIENT_BLOCKDATA, 2 + 2 + 2 + s.size(), peer_id);

	pkt << block->getPos();
	pkt.putRawString(s.c_str(), s.size());
	Send(&pkt);
}

void Server::SendBlockNoLock(maskedu16 playerid, MapBlock *block, u8 ver, u16 net_proto_version)
{
	/*
		Create a packet with the block in the right format
	*/
	thread_local const int net_compression_level = rangelim(g_settings->getS16("map_compression_level_net"), -1, 9);
	std::ostringstream os(std::ios_base::binary);

	RemotePlayer *player = m_env->getPlayer(playerid);
	
	if (player && player->protocol_version < 37)
		block->serialize(os, ver, false, net_compression_level, player->formspec_prepend);
	else
		block->serialize(os, ver, false, net_compression_level, std::string(""), Jumper);
	
	block->serializeNetworkSpecific(os);
	std::string s = os.str();

	u16 SrvID = ServersNetworkObject->QueryThisServerID();

	NetworkPacket pkt(TOCLIENT_BLOCKDATA, 9 + s.size()); // {2+2+2} + 2 + 1 + S_STRING

	pkt << (u8)SrvID;
	pkt << playerid.value;

	pkt << block->getPos();
	pkt.putRawString(s.c_str(), s.size());
	Send(&pkt);
}


void Server::SendBlocks(float dtime)
{
	MutexAutoLock envlock(m_env_mutex);
	//TODO check if one big lock could be faster then multiple small ones
	
	
	if (!ServersNetworkObject->AreSlave) {
		std::vector<PrioritySortedBlockTransfer> queue;
		u32 total_sending = 0;
		{
			ScopeProfiler sp2(g_profiler, "Server::SendBlocks(): Collect list");
			std::vector<session_t> clients = m_clients.getClientIDs();
			m_clients.lock();
			for (const session_t client_id : clients) {
				RemoteClient *client = m_clients.lockedGetClientNoEx(client_id, CS_Active);
				if (!client)
					continue;
				total_sending += client->getSendingCount();
				client->GetNextBlocks(m_env, m_emerge, dtime, queue);
			}
			m_clients.unlock();
		}
		// Sort.
		// Lowest priority number comes first.
		// Lowest is most important.
		std::sort(queue.begin(), queue.end());
		m_clients.lock();
		// Maximal total count calculation
		// The per-client block sends is halved with the maximal online users
		u32 max_blocks_to_send = (m_env->getPlayerCount() + g_settings->getU32("max_users")) * g_settings->getU32("max_simultaneous_block_sends_per_client") / 4 + 1;
		ScopeProfiler sp(g_profiler, "Server::SendBlocks(): Send to clients");
		Map &map = m_env->getMap();

		for (const PrioritySortedBlockTransfer &block_to_send : queue) {
			if (total_sending >= max_blocks_to_send)
				break;

			MapBlock *block = map.getBlockNoCreateNoEx(block_to_send.pos);
			if (!block)
				continue;

			RemoteClient *client = m_clients.lockedGetClientNoEx(block_to_send.peer_id,
					CS_Active);
			if (!client)
				continue;

			SendBlockNoLock(block_to_send.peer_id, block, client->serialization_version, client->net_proto_version);

			client->SentBlock(block_to_send.pos);
			total_sending++;
		}
		m_clients.unlock();
	} else { //is slave
		std::vector<PrioritySortedBlockTransferPID> queue;
		u32 total_sending = 0;
		{
			ScopeProfiler sp2(g_profiler, "Server::SendBlocks(): Collect list");
			for (auto it = ClientDataTable.begin(); it != ClientDataTable.end(); ++it) {
				ClientDataHelper *client = it->second;
				if (!client)
					continue;
				total_sending += client->getSendingCount();
				client->GetNextBlocks(m_env, m_emerge, dtime, queue);
			}
		}
		std::sort(queue.begin(), queue.end());
		u32 max_blocks_to_send = (m_env->getPlayerCount() + g_settings->getU32("max_users")) * g_settings->getU32("max_simultaneous_block_sends_per_client") / 4 + 1;
		ScopeProfiler sp(g_profiler, "Server::SendBlocks(): Send to clients");
		Map &map = m_env->getMap();
		//send
		for (const PrioritySortedBlockTransferPID &block_to_send : queue) {
			if (total_sending >= max_blocks_to_send)
				break;
			MapBlock *block = map.getBlockNoCreateNoEx(block_to_send.pos);
			if (!block)
				continue;
			
			if (ClientDataTable.find(block_to_send.playerid) == ClientDataTable.end())
				continue;
			
			ClientDataHelper *client = ClientDataTable[block_to_send.playerid];
			
			SendBlockNoLock(maskedu16(block_to_send.playerid), block, client->serialization_version, client->net_proto_version);
			client->SentBlock(block_to_send.pos);
			total_sending++;
		}
	}
}

bool Server::SendBlock(session_t peer_id, const v3s16 &blockpos)
{
	MapBlock *block = m_env->getMap().getBlockNoCreateNoEx(blockpos);
	if (!block)
		return false;

	m_clients.lock();
	RemoteClient *client = m_clients.lockedGetClientNoEx(peer_id, CS_Active);
	if (!client || client->isBlockSent(blockpos)) {
		m_clients.unlock();
		return false;
	}
	SendBlockNoLock(peer_id, block, client->serialization_version,
			client->net_proto_version);
	m_clients.unlock();

	return true;
}

//but u16
bool Server::SendBlock(maskedu16 pid, const v3s16 &blockpos)
{
	MapBlock *block = m_env->getMap().getBlockNoCreateNoEx(blockpos);
	if (!block)
		return false;
	
	if (ClientDataTable.find(pid.value) == ClientDataTable.end())
		return false;
	
	ClientDataHelper *client = ClientDataTable[pid.value];
	
	if (!client || client->isBlockSent(blockpos)) {
		return false;
	}
	SendBlockNoLock(pid, block, client->serialization_version, client->net_proto_version);

	return true;
}

//sendConnect of ServerNetworkEngine requires a serialization version to send to a slave server
u8 Server::getSerializationVersion(session_t peer_id) {
	RemoteClient *client = m_clients.lockedGetClientNoEx(peer_id, CS_Active);
	if (!client)
		return SER_FMT_VER_INVALID;
	return client->serialization_version;
}

// Hacks because I don't want to make duplicate read/write functions for little
// endian numbers.
u32 readU32_le(std::istream &is) {
	char buf[4] = {0};
	is.read(buf, sizeof(buf));
	std::reverse(buf, buf + sizeof(buf));
	return readU32((u8 *)buf);
}

v3f readV3F32_le(std::istream &is) {
	char buf[12] = {0};
	is.read(buf, sizeof(buf));
	std::reverse(buf, buf + 4);
	std::reverse(buf + 4, buf + 8);
	std::reverse(buf + 8, buf + 12);
	return readV3F32((u8 *)buf);
}

void writeV3F32_le(std::ostream &os, v3f pos) {
	char buf[12];
	writeV3F32((u8 *)buf, pos);
	std::reverse(buf, buf + 4);
	std::reverse(buf + 4, buf + 8);
	std::reverse(buf + 8, buf + 12);
	os.write(buf, sizeof(buf));
}

// Converts MT 5+ player models into MT 0.4 compatible models
std::string makeCompatPlayerModel(std::string b3d) {
	std::stringstream ss(b3d);

	// ss.read(4) != "BB3D"
	const u32 header = readU32_le(ss);
	if (header != 0x44334242) {
		warningstream << "Invalid B3D header in player model: " << header << std::endl;
		return "";
	}

	readU32(ss); // Length
	readU32(ss); // Version

	// Look for the node
	while (ss.good()) {
		const u32 name = readU32_le(ss);
		const u32 length = readU32_le(ss);

		// name != "NODE"
		if (name != 0x45444f4e) {
			ss.ignore(length);
			continue;
		}

		// Node name
		ss.ignore(length, '\x00');

		// Node position
		std::streampos p = ss.tellg();
		const v3f offset_pos = readV3F32_le(ss) - v3f(0, BS, 0);

		// Write the new position back to the stringstream
		ss.seekp(p);
		writeV3F32_le(ss, offset_pos);

		return ss.str();
	}

	warningstream << "Could not find base position in B3D file" << std::endl;
	return "";
}

bool Server::addMediaFile(const std::string &filename,
	const std::string &filepath, std::string *filedata_to,
	std::string *digest_to)
{
	// If name contains illegal characters, ignore the file
	if (!string_allowed(filename, TEXTURENAME_ALLOWED_CHARS)) {
		infostream << "Server: ignoring illegal file name: \""
				<< filename << "\"" << std::endl;
		return false;
	}
	// If name is not in a supported format, ignore it
	const char *supported_ext[] = {
		".png", ".jpg", ".bmp", ".tga",
		".pcx", ".ppm", ".psd", ".wal", ".rgb",
		".ogg",
		".x", ".b3d", ".md2", ".obj",
		// Custom translation file format
		".tr",
		".e",
		NULL
	};
	if (removeStringEnd(filename, supported_ext).empty()) {
		infostream << "Server: ignoring unsupported file extension: \""
				<< filename << "\"" << std::endl;
		return false;
	}
	// Ok, attempt to load the file and add to cache

	// Read data
	std::string filedata;
	if (!fs::ReadFile(filepath, filedata)) {
		errorstream << "Server::addMediaFile(): Failed to open \""
					<< filepath << "\" for reading" << std::endl;
		return false;
	}

	if (filedata.empty()) {
		errorstream << "Server::addMediaFile(): Empty file \""
				<< filepath << "\"" << std::endl;
		return false;
	}

	SHA1 sha1;
	sha1.addBytes(filedata.c_str(), filedata.length());

	unsigned char *digest = sha1.getDigest();
	std::string sha1_base64 = base64_encode(digest, 20);
	if (digest_to)
		*digest_to = std::string((char*) digest, 20);
	free(digest);

	// Put in list
	m_media[filename] = MediaInfo(filepath, sha1_base64);

	// Add a compatibility model if required
	if (isCompatPlayerModel(filename)) {
		// Offset the mesh
		const std::string filedata_compat = makeCompatPlayerModel(filedata);
		if (filedata_compat != "") {
			SHA1 sha1;
			sha1.addBytes(filedata_compat.c_str(), filedata_compat.length());
			unsigned char *digest = sha1.getDigest();
			std::string sha1_base64 = base64_encode(digest, 20);
			free(digest);

			// If the original model is being sent then rename the
			// compatibility one so it doesn't conflict. The renamed model is
			// used in player_sao.cpp if the setting is enabled.
			std::string fn_compat = filename;
			if (g_settings->getBool("compat_send_original_model")) {
				fn_compat = "_mc_compat_" + fn_compat;

				// Add a dummy m_media entry
				m_media[fn_compat] = MediaInfo("", "");
			}

			m_compat_media[fn_compat] = InMemoryMediaInfo(filedata_compat, sha1_base64);
		}
	}

	if (filedata_to)
		*filedata_to = std::move(filedata);
	return true;
}

void Server::fillMediaCache()
{
	infostream << "Server: Calculating media file checksums" << std::endl;

	// Collect all media file paths
	std::vector<std::string> paths;
	// The paths are ordered in descending priority
	fs::GetRecursiveDirs(paths, porting::path_user + DIR_DELIM + "textures" + DIR_DELIM + "server");
	fs::GetRecursiveDirs(paths, m_gamespec.path + DIR_DELIM + "textures");
	m_modmgr->getModsMediaPaths(paths);

	// Collect media file information from paths into cache
	for (const std::string &mediapath : paths) {
		std::vector<fs::DirListNode> dirlist = fs::GetDirListing(mediapath);
		for (const fs::DirListNode &dln : dirlist) {
			if (dln.dir) // Ignore dirs (already in paths)
				continue;

			const std::string &filename = dln.name;
			if (m_media.find(filename) != m_media.end()) // Do not override
				continue;

			std::string filepath = mediapath;
			filepath.append(DIR_DELIM).append(filename);
			addMediaFile(filename, filepath);
		}
	}

	infostream << "Server: " << m_media.size() << " media files collected" << std::endl;
}

void Server::sendMediaAnnouncement(session_t peer_id, const std::string &lang_code)
{
	const u16 protocol_version = m_clients.getProtocolVersion(peer_id);

	TimeTaker timer("Send Media");

	// Make packet
	NetworkPacket pkt(TOCLIENT_ANNOUNCE_MEDIA, 0, peer_id);

	u16 media_sent = 0;
	std::string lang_suffix;
	lang_suffix.append(".").append(lang_code).append(".tr");
	for (const auto &i : m_media) {
		if (str_ends_with(i.first, ".tr") && !str_ends_with(i.first, lang_suffix))
			continue;
		if (str_ends_with(i.first, ".tr.e") && !str_ends_with(i.first, lang_suffix + ".e"))
			continue;
		// Skip dummy entries on 5.0+ clients
		if (protocol_version >= 37 && i.second.sha1_digest.empty())
			continue;
		media_sent++;
	}

	std::unordered_map<std::string, MediaInfo> external_media = ServersNetworkObject->getMedia();
	for (const auto &i: external_media) {
		if (str_ends_with(i.first, ".tr") && !str_ends_with(i.first, lang_suffix))
			continue;
		if (str_ends_with(i.first, ".tr.e") && !str_ends_with(i.first, lang_suffix + ".e"))
			continue;
		if (m_media.find(i.first) != m_media.end()) //Somehow some media continues appearing here
			continue;
		if ((u32)media_sent + 1 >= UINT16_MAX)
			FATAL_ERROR("Media Overflow");
		media_sent++;
	}

	pkt << media_sent;

	for (const auto &i : m_media) {
		if (str_ends_with(i.first, ".tr") && !str_ends_with(i.first, lang_suffix))
			continue;
		if (str_ends_with(i.first, ".tr.e") && !str_ends_with(i.first, lang_suffix + ".e"))
			continue;
		if (protocol_version >= 37 && i.second.sha1_digest.empty())
			continue;

		pkt << i.first;

		if (protocol_version < 37 &&
				m_compat_media.find(i.first) != m_compat_media.end()) {
			pkt << m_compat_media[i.first].sha1_digest;
		} else {
			FATAL_ERROR_IF(i.second.sha1_digest.empty(), "Attempt to send dummy media");
			pkt << i.second.sha1_digest;
		}
	}

	
	for (const auto &i: external_media) {
		if (str_ends_with(i.first, ".tr") && !str_ends_with(i.first, lang_suffix))
			continue;
		if (str_ends_with(i.first, ".tr.e") && !str_ends_with(i.first, lang_suffix + ".e"))
			continue;
		verbosestream << "MediaVerifSend: " << i.first << ", sha=" << i.second.sha1_digest << std::endl;
		pkt << i.first;
		pkt << i.second.sha1_digest;
	}

	pkt << g_settings->get("remote_media");
	if (g_settings->getBool("disable_texture_packs"))
		pkt << true;

	Send(&pkt);

	verbosestream << "Server: Announcing files to id(" << peer_id
		<< "): count=" << media_sent << " size=" << pkt.getSize() << std::endl;
	//warningstream << "Took " << timer.getTimerTime() << "ms" << std::endl;
}

struct SendableMedia
{
	std::string name;
	std::string path;
	std::string data;

	SendableMedia(const std::string &name_="", const std::string &path_="",
	              const std::string &data_=""):
		name(name_),
		path(path_),
		data(data_)
	{}
};

void Server::sendRequestedMedia(session_t peer_id,
		const std::vector<std::string> &tosend)
{
	verbosestream<<"Server::sendRequestedMedia(): "
			<<"Sending files to client"<<std::endl;
	TimeTaker timer("Send Requested Media");

	/* Read files */

	// Put 5kB in one bunch (this is not accurate)
	u32 bytes_per_bunch = 5000;

	std::vector< std::vector<SendableMedia> > file_bunches;
	file_bunches.emplace_back();

	u32 file_size_bunch_total = 0;

	std::unordered_map<std::string, MediaInfo> external_media = ServersNetworkObject->getMedia();

	const u16 protocol_version = m_clients.getProtocolVersion(peer_id);
	for (const std::string &name : tosend) {
		if ((m_media.find(name) == m_media.end()) && !ServersNetworkObject->HasMedia(name)) {
			errorstream<<"Server::sendRequestedMedia(): Client asked for "
					<<"unknown file \""<<(name)<<"\""<<std::endl;
			continue;
		}

		//TODO get path + name
		std::string tpath = m_media[name].path;
		if (tpath.empty() && ServersNetworkObject->HasMedia(name)) {
			tpath = external_media[name].path;
		}

		// Use compatibility media on older clients
		if (protocol_version < 37 &&
				m_compat_media.find(name) != m_compat_media.end()) {
			file_bunches[file_bunches.size()-1].emplace_back(name, tpath,
					m_compat_media[name].data);
			continue;
		}

		if (tpath.empty()) {
			errorstream<<"Server::sendRequestedMedia(): New client asked for "
					<<"compatibility media file \""<<(name)<<"\""<<std::endl;
			continue;
		}

		// Read data
		std::ifstream fis(tpath.c_str(), std::ios_base::binary);
		if(!fis.good()){
			errorstream<<"Server::sendRequestedMedia(): Could not open \""
					<<tpath<<"\" for reading"<<std::endl;
			continue;
		}
		std::ostringstream tmp_os(std::ios_base::binary);
		bool bad = false;
		for(;;) {
			char buf[1024];
			fis.read(buf, 1024);
			std::streamsize len = fis.gcount();
			tmp_os.write(buf, len);
			file_size_bunch_total += len;
			if(fis.eof())
				break;
			if(!fis.good()) {
				bad = true;
				break;
			}
		}
		if (bad) {
			errorstream<<"Server::sendRequestedMedia(): Failed to read \""
					<<name<<"\""<<std::endl;
			continue;
		}
		/*infostream<<"Server::sendRequestedMedia(): Loaded \""
				<<tname<<"\""<<std::endl;*/
		// Put in list
		file_bunches[file_bunches.size()-1].emplace_back(name, tpath, tmp_os.str());

		// Start next bunch if got enough data
		if(file_size_bunch_total >= bytes_per_bunch) {
			file_bunches.emplace_back();
			file_size_bunch_total = 0;
		}

	}

	/* Create and send packets */
	u16 num_bunches = file_bunches.size();
	for (u16 i = 0; i < num_bunches; i++) {
		/*
			u16 command
			u16 total number of texture bunches
			u16 index of this bunch
			u32 number of files in this bunch
			for each file {
				u16 length of name
				string name
				u32 length of data
				data
			}
		*/

		NetworkPacket pkt(TOCLIENT_MEDIA, 4 + 0, peer_id);
		pkt << num_bunches << i << (u32) file_bunches[i].size();

		for (const SendableMedia &j : file_bunches[i]) {
			pkt << j.name;
			pkt.putLongString(j.data);
		}

		verbosestream << "Server::sendRequestedMedia(): bunch "
				<< i << "/" << num_bunches
				<< " files=" << file_bunches[i].size()
				<< " size="  << pkt.getSize() << std::endl;
		Send(&pkt);
	}
}

void Server::SendMinimapModes(session_t peer_id,
		std::vector<MinimapMode> &modes, size_t wanted_mode)
{
	RemotePlayer *player = m_env->getPlayer(peer_id);
	assert(player);
	if (player->getPeerId() == PEER_ID_INEXISTENT)
		return;

	NetworkPacket pkt(TOCLIENT_MINIMAP_MODES, 0, peer_id);
	pkt << (u16)modes.size() << (u16)wanted_mode;

	for (auto &mode : modes)
		pkt << (u16)mode.type << mode.label << mode.size << mode.texture << mode.scale;

	Send(&pkt);
}

void Server::SendMinimapModes(maskedu16 pid, std::vector<MinimapMode> &modes, size_t wanted_mode) {
	RemotePlayer *player = m_env->getPlayer(pid);
	assert(player);

	NetworkPacket pkt(TOCLIENT_MINIMAP_MODES, 2+1);
	pkt << (u8)ServersNetworkObject->QueryThisServerID() << pid.value << (u16)modes.size() << (u16)wanted_mode;

	for (auto &mode : modes)
		pkt << (u16)mode.type << mode.label << mode.size << mode.texture << mode.scale;

	Send(&pkt);
}

void Server::sendDetachedInventory(Inventory *inventory, const std::string &name, session_t peer_id)
{
	NetworkPacket pkt(TOCLIENT_DETACHED_INVENTORY, 0, peer_id);
	NetworkPacket legacy_pkt(TOCLIENT_DETACHED_INVENTORY, 0, peer_id);
	pkt << name;
	legacy_pkt << name;

	if (!inventory) {
		pkt << false; // Remove inventory
	} else {
		pkt << true; // Update inventory

		// Serialization & NetworkPacket isn't a love story
		std::ostringstream os(std::ios_base::binary);
		inventory->serialize(os);
		inventory->setModified(false);

		const std::string &os_str = os.str();
		pkt << static_cast<u16>(os_str.size()); // HACK: to keep compatibility with 5.0.0 clients
		pkt.putRawString(os_str);
		legacy_pkt.putRawString(os_str);
	}

	if (peer_id == PEER_ID_INEXISTENT) {
		m_clients.newSendToAll(&pkt);
		if (inventory)
			m_clients.oldSendToAll(&legacy_pkt);
	} else {
		RemoteClient *client = getClientNoEx(peer_id, CS_Created);
		if (!client) {
			warningstream << "Could not get client in sendDetachedInventory!"
				<< std::endl;
		}

		if (!client || client->net_proto_version >= 37)
			Send(&pkt);
		else if (inventory)
			Send(&legacy_pkt);
	}
}

//LL3686
void Server::sendDetachedInventory(Inventory *inventory, const std::string &name, maskedu16 PLAYER_ID)
{
	NetworkPacket pkt(TOCLIENT_DETACHED_INVENTORY, 0);
	pkt << (u8) ServersNetworkObject->QueryThisServerID();
	pkt << PLAYER_ID.value; //may not needed at full, 0 must be specified if it will be sent to everyone
	pkt << name;

	if (!inventory) {
		pkt << false; // Remove inventory
	} else {
		pkt << true; // Update inventory
		// Serialization & NetworkPacket isn't a love story
		std::ostringstream os(std::ios_base::binary);
		inventory->serialize(os);
		inventory->setModified(false);
		const std::string &os_str = os.str();
		pkt << static_cast<u16>(os_str.size()); // HACK: to keep compatibility with 5.0.0 clients
		pkt.putRawString(os_str);
	}
	
	Send(&pkt);
}

void Server::sendDetachedInventories(session_t peer_id, bool incremental)
{
	// Lookup player name, to filter detached inventories just after
	std::string peer_name;
	if (peer_id != PEER_ID_INEXISTENT) {
		peer_name = getClient(peer_id, CS_Created)->getName();
	}

	auto send_cb = [this, peer_id](const std::string &name, Inventory *inv) {
		sendDetachedInventory(inv, name, peer_id);
	};

	m_inventory_mgr->sendDetachedInventories(peer_name, incremental, send_cb);
}

void Server::sendDetachedInventories(maskedu16 player_id, bool incremental)
{
	// Lookup player name, to filter detached inventories just after
	std::string fromIDtoname;
	if (player_id.value != 0) {
		RemotePlayer *player = m_env->getPlayer(player_id);
		fromIDtoname = (std::string)player->getName();
	}

	auto send_cb = [this, player_id] (const std::string &name, Inventory *inv) {
		sendDetachedInventory(inv, name, player_id.value);
	};

	m_inventory_mgr->sendDetachedInventories(fromIDtoname, incremental, send_cb);
}

/*
	Something random
*/

void Server::DiePlayer(session_t peer_id, const PlayerHPChangeReason &reason)
{
	PlayerSAO *playersao = getPlayerSAO(peer_id);
	assert(playersao);

	infostream << "Server::DiePlayer(): Player "
			<< playersao->getPlayer()->getName()
			<< " dies" << std::endl;

	playersao->setHP(0, reason);
	playersao->clearParentAttachment();

	// Trigger scripted stuff
	m_script->on_dieplayer(playersao, reason);

	SendPlayerHP(peer_id);
	SendDeathscreen(peer_id, false, v3f(0,0,0));
}

void Server::DiePlayer(maskedu16 pid, const PlayerHPChangeReason &reason) {
	PlayerSAO *playersao = getPlayerSAO(pid);
	assert(playersao);

	infostream << "Server::DiePlayer(): Player " << playersao->getPlayer()->getName() << " dies" << std::endl;

	playersao->setHP(0, reason);
	playersao->clearParentAttachment();

	// Trigger scripted stuff
	m_script->on_dieplayer(playersao, reason);

	SendPlayerHP(pid);
	SendDeathscreen(pid, false, v3f(0,0,0));
}

void Server::RespawnPlayer(session_t peer_id)
{
	PlayerSAO *playersao = getPlayerSAO(peer_id);
	assert(playersao);

	infostream << "Server::RespawnPlayer(): Player "
			<< playersao->getPlayer()->getName()
			<< " respawns" << std::endl;

	playersao->setHP(playersao->accessObjectProperties()->hp_max,
			PlayerHPChangeReason(PlayerHPChangeReason::RESPAWN));
	playersao->setBreath(playersao->accessObjectProperties()->breath_max);

	bool repositioned = m_script->on_respawnplayer(playersao);
	if (!repositioned) {
		// setPos will send the new position to client
		const v3f pos = findSpawnPos();
		actionstream << "Moving " << playersao->getPlayer()->getName() <<
				" to spawnpoint at (" << pos.X << ", " << pos.Y << ", " <<
				pos.Z << ")" << std::endl;
		playersao->setPos(pos);
	}

	SendPlayerHP(peer_id);
}


void Server::RespawnPlayer(maskedu16 PID) {
	PlayerSAO *playersao = getPlayerSAO(PID);
	assert(playersao);
	infostream << "Server::RespawnPlayer(): Player " << playersao->getPlayer()->getName() << " respawns" << std::endl;
	playersao->setHP(playersao->accessObjectProperties()->hp_max, PlayerHPChangeReason(PlayerHPChangeReason::RESPAWN));
	playersao->setBreath(playersao->accessObjectProperties()->breath_max);
	bool repositioned = m_script->on_respawnplayer(playersao);
	if (!repositioned) {
		// setPos will send the new position to client
		const v3f pos = findSpawnPos();
		actionstream << "Moving " << playersao->getPlayer()->getName() << " to spawnpoint at (" << pos.X << ", " << pos.Y << ", " <<pos.Z << ")" << std::endl;
		playersao->setPos(pos);
	}
	SendPlayerHP(PID);
}


void Server::DenySudoAccess(session_t peer_id)
{
	NetworkPacket pkt(TOCLIENT_DENY_SUDO_MODE, 0, peer_id);
	Send(&pkt);
}


void Server::DenyAccess(session_t peer_id, AccessDeniedCode reason,
		const std::string &custom_reason, bool reconnect)
{
	SendAccessDenied(peer_id, reason, custom_reason, reconnect);
	m_clients.event(peer_id, CSE_SetDenied);
	DisconnectPeer(peer_id);
}

void Server::DisconnectPeer(session_t peer_id)
{
	m_modchannel_mgr->leaveAllChannels(peer_id);
	m_con->DisconnectPeer(peer_id);
}

void Server::acceptAuth(session_t peer_id, bool forSudoMode)
{
	if (!forSudoMode) {
		RemoteClient* client = getClient(peer_id, CS_Invalid);

		NetworkPacket resp_pkt(TOCLIENT_AUTH_ACCEPT, 1 + 6 + 8 + 4, peer_id, client->net_proto_version);

		// Right now, the auth mechs don't change between login and sudo mode.
		u32 sudo_auth_mechs = client->allowed_auth_mechs;
		client->allowed_sudo_mechs = sudo_auth_mechs;

		resp_pkt << v3f(0,0,0) << (u64) m_env->getServerMap().getSeed()
				<< g_settings->getFloat("dedicated_server_step")
				<< sudo_auth_mechs;

		Send(&resp_pkt);
		m_clients.event(peer_id, CSE_AuthAccept);
	} else {
		NetworkPacket resp_pkt(TOCLIENT_ACCEPT_SUDO_MODE, 1 + 6 + 8 + 4, peer_id);

		// We only support SRP right now
		u32 sudo_auth_mechs = AUTH_MECHANISM_FIRST_SRP;

		resp_pkt << sudo_auth_mechs;
		Send(&resp_pkt);
		m_clients.event(peer_id, CSE_SudoSuccess);
	}
}

void Server::DeleteClient(session_t peer_id, ClientDeletionReason reason)
{
	bool was_in_slave = false;
	std::wstring message;
	{
		bool __ = SessionsStatus.find(peer_id) != SessionsStatus.end();
		if (__ && SessionsStatus[peer_id]) {
			//Send disconnect to a server
			RemotePlayer *player = m_env->getPlayer(peer_id);
			u16 SrvID = player->OnServer->ID;
			ServersNetworkObject->SendDisconnectONLY(player->player_id, SrvID);
			SessionsStatus.erase(peer_id);
			was_in_slave = true;
			//return;
		}
		/*
			Clear references to playing sounds
		*/
		for (std::unordered_map<s32, ServerPlayingSound>::iterator
				 i = m_playing_sounds.begin(); i != m_playing_sounds.end();) {
			ServerPlayingSound &psound = i->second;
			psound.clients.erase(peer_id);
			if (psound.clients.empty())
				m_playing_sounds.erase(i++);
			else
				++i;
		}

		// clear formspec info so the next client can't abuse the current state
		m_formspec_state_data.erase(peer_id);

		RemotePlayer *player = m_env->getPlayer(peer_id);

		/* Run scripts and remove from environment */
		if (player) {
			PlayerSAO *playersao = player->getPlayerSAO();
			//assert(playersao); //Don't crash if SAO is not found, the player might be playing on a slave server, so the SAO do not exist
			if (playersao) {

				playersao->clearChildAttachments();
				playersao->clearParentAttachment();

				// inform connected clients
				const std::string &player_name = player->getName();
				NetworkPacket notice(TOCLIENT_UPDATE_PLAYER_LIST, 0, PEER_ID_INEXISTENT);
				// (u16) 1 + std::string represents a vector serialization representation
				notice << (u8) PLAYER_LIST_REMOVE  << (u16) 1 << player_name;
				m_clients.sendToAll(&notice);
				// run scripts
				m_script->on_leaveplayer(playersao, reason == CDR_TIMEOUT);

				playersao->disconnected();
			}
		}

		/*
			Print out action
		*/
		{
			if (player && reason != CDR_DENY) {
				std::ostringstream os(std::ios_base::binary);
				std::vector<session_t> clients = m_clients.getClientIDs();

				for (const session_t client_id : clients) {
					// Get player
					RemotePlayer *player = m_env->getPlayer(client_id);
					if (!player)
						continue;

					// Get name of player
					os << player->getName() << " ";
				}

				std::string name = player->getName();
				actionstream << name << " "
						<< (reason == CDR_TIMEOUT ? "times out." : " leaves game.")
						<< " List of players: " << os.str() << std::endl;
				if (m_admin_chat)
					m_admin_chat->outgoing_queue.push_back(
						new ChatEventNick(CET_NICK_REMOVE, name));
			}
		}
		{
			MutexAutoLock env_lock(m_env_mutex);
			m_clients.DeleteClient(peer_id);
		}
	}

	// Send leave chat message to all remaining clients
	if (!message.empty()) {
		if (!was_in_slave) {
			if (!ServersNetworkObject->AreSlave)
				SendChatMessage(PEER_ID_INEXISTENT, ChatMessage(CHATMESSAGE_TYPE_ANNOUNCE, message));
			else
				SendChatMessage(maskedu16(), ChatMessage(CHATMESSAGE_TYPE_ANNOUNCE, message));
		}
	}
	
	//Delete some parts
	//auto it = SessionsStatus.find(peer_id);
	//if (it != SessionsStatus.end())
		//ServersNetworkObject->SendDisconnect(SessionsToU16[peer_id], PlayerInEnvironmentToServer[peer_id]);
	//SessionsStatus.erase(peer_id);
	PlayerInEnvironmentToServer.erase(peer_id);
	ActivePlayersU16.erase(SessionsToU16[peer_id]);
	SessionsToU16.erase(peer_id);
}

void Server::OnlyDeleteSAO(session_t peer_id, u16 ID_) {
	//Remove sounds
	for (std::unordered_map<s32, ServerPlayingSound>::iterator i = m_playing_sounds.begin(); i != m_playing_sounds.end();) {
		ServerPlayingSound &psound = i->second;
		psound.clients.erase(peer_id);
		if (psound.clients.empty())
			m_playing_sounds.erase(i++);
		else
			++i;
	}
	m_formspec_state_data.erase(peer_id);
	RemotePlayer *player = m_env->getPlayer(peer_id);
	/* Run scripts and remove from environment */
	if (player) {
		PlayerSAO *playersao = player->getPlayerSAO();
		assert(playersao);
		playersao->clearChildAttachments();
		playersao->clearParentAttachment();
		// inform connected clients
		const std::string &player_name = player->getName();
		NetworkPacket notice(TOCLIENT_UPDATE_PLAYER_LIST, 0, PEER_ID_INEXISTENT);
		// (u16) 1 + std::string represents a vector serialization representation
		notice << (u8) PLAYER_LIST_REMOVE  << (u16) 1 << player_name;
		m_clients.sendToAll(&notice);
		// run scripts
		m_script->on_leaveplayer(playersao, CDR_LEAVE);
		playersao->disconnected(true);
	}
	SessionsStatus[peer_id] = true;
	PlayerInEnvironmentToServer[peer_id] = ID_;
	actionstream << "a player leaves the server [Going to other server] [" << player->getName() << "]" << std::endl;

	//SendRemoveObjectsToClient(player->player_id);
	//SendChatMessage(peer_id, ChatMessage(CHATMESSAGE_TYPE_ANNOUNCE, message)); //MODIFY
}

void Server::RunScriptsOnLeave(session_t PID) {
	//RemotePlayer *p = m_env->getPlayer(PID);
	//PlayerSAO *playersao = p->getPlayerSAO();
	
}

void Server::DeleteClientU16(u16 pid, ClientDeletionReason reason)
{
	std::wstring message;
	{
		/*
			Clear references to playing sounds
		*/
		for (std::unordered_map<s32, ServerPlayingSound>::iterator i = m_playing_sounds.begin(); i != m_playing_sounds.end();) {
			ServerPlayingSound &psound = i->second;
			psound.clients_int16.erase(pid);
			if (psound.clients.empty())
				m_playing_sounds.erase(i++);
			else
				++i;
		}

		// clear formspec info so the next client can't abuse the current state
		m_formspec_state_dataU16.erase(pid);

		RemotePlayer *player = m_env->getPlayer(pid);

		/* Run scripts and remove from environment */
		if (player) {
			PlayerSAO *playersao = player->getPlayerSAO();
			assert(playersao);

			playersao->clearChildAttachments();
			playersao->clearParentAttachment();

			// inform connected clients
			const std::string &player_name = player->getName();
			
			NetworkPacket notice(TOCLIENT_UPDATE_PLAYER_LIST, 0, PEER_ID_INEXISTENT);
			// (u16) 1 + std::string represents a vector serialization representation
			notice << (u8) ServersNetworkObject->QueryThisServerID() << (u8) PLAYER_LIST_REMOVE  << (u16) 1 << player_name;
			Send(&notice);
			
			// run scripts
			m_script->on_leaveplayer(playersao, reason == CDR_TIMEOUT);

			playersao->disconnected();
		}

		/*
			Print out action
		*/
		{
			if (player && reason != CDR_DENY) {
				std::string name = player->getName();
				actionstream << name << " " << (reason == CDR_TIMEOUT ? "times out." : " leaves game.") << std::endl;
			}
		}
		ClientDataTable.erase(pid);
	}

	// Send leave chat message to all remaining clients
	if (!message.empty()) {
		if (!ServersNetworkObject->AreSlave)
			SendChatMessage(PEER_ID_INEXISTENT, ChatMessage(CHATMESSAGE_TYPE_ANNOUNCE, message));
		else
			SendChatMessage(maskedu16(), ChatMessage(CHATMESSAGE_TYPE_ANNOUNCE, message));
	}
}

void Server::UpdateCrafting(RemotePlayer *player)
{
	InventoryList *clist = player->inventory.getList("craft");
	if (!clist || clist->getSize() == 0)
		return;

	if (!clist->checkModified())
		return;

	// Get a preview for crafting
	ItemStack preview;
	InventoryLocation loc;
	loc.setPlayer(player->getName());
	std::vector<ItemStack> output_replacements;
	getCraftingResult(&player->inventory, preview, output_replacements, false, this);
	m_env->getScriptIface()->item_CraftPredict(preview, player->getPlayerSAO(),
			clist, loc);

	InventoryList *plist = player->inventory.getList("craftpreview");
	if (plist && plist->getSize() >= 1) {
		// Put the new preview in
		plist->changeItem(0, preview);
	}
}

void Server::handleChatInterfaceEvent(ChatEvent *evt)
{
	if (evt->type == CET_NICK_ADD) {
		// The terminal informed us of its nick choice
		m_admin_nick = ((ChatEventNick *)evt)->nick;
		if (!m_script->getAuth(m_admin_nick, NULL, NULL)) {
			errorstream << "You haven't set up an account." << std::endl
				<< "Please log in using the client as '"
				<< m_admin_nick << "' with a secure password." << std::endl
				<< "Until then, you can't execute admin tasks via the console," << std::endl
				<< "and everybody can claim the user account instead of you," << std::endl
				<< "giving them full control over this server." << std::endl;
		}
	} else {
		assert(evt->type == CET_CHAT);
		handleAdminChat((ChatEventChat *)evt);
	}
}

std::wstring Server::handleChat(const std::string &name, std::wstring wmessage, bool check_shout_priv, RemotePlayer *player)
{
#if USE_SQLITE
	// If something goes wrong, this player is to blame
	RollbackScopeActor rollback_scope(m_rollback,
			std::string("player:") + name);
#endif

	if (g_settings->getBool("strip_color_codes"))
		wmessage = unescape_enriched(wmessage);

	const bool sscsm_com = wmessage.find(L"/admin \x01SSCSM_COM\x01", 0) == 0;
	if (player && !sscsm_com) {
		switch (player->canSendChatMessage()) {
		case RPLAYER_CHATRESULT_FLOODING: {
			std::wstringstream ws;
			ws << L"You cannot send more messages. You are limited to "
					<< g_settings->getFloat("chat_message_limit_per_10sec")
					<< L" messages per 10 seconds.";
			return ws.str();
		}
		case RPLAYER_CHATRESULT_KICK:
			DenyAccess(player->getPeerId(), SERVER_ACCESSDENIED_CUSTOM_STRING,
				"You have been kicked due to message flooding.");
			return L"";
		case RPLAYER_CHATRESULT_OK:
			break;
		default:
			FATAL_ERROR("Unhandled chat filtering result found.");
		}
	}

	if (m_max_chatmessage_length > 0
			&& wmessage.length() > m_max_chatmessage_length) {
		return L"Your message exceed the maximum chat message limit set on the server. "
				L"It was refused. Send a shorter message";
	}

	auto message = trim(wide_to_utf8(wmessage));
	if (message.find_first_of("\n\r") != std::wstring::npos) {
		return L"Newlines are not permitted in chat messages";
	}

	// Run script hook, exit if script ate the chat message
	if (m_script->on_chat_message(name, message)) {
		if (!sscsm_com)
			actionstream << name << " issued command: " << message << std::endl;
		return L"";
	}

	// Line to send
	std::wstring line;
	// Whether to send line to the player that sent the message, or to all players
	bool broadcast_line = true;

	if (check_shout_priv && !checkPriv(name, "shout")) {
		line += L"-!- You don't have permission to shout.";
		broadcast_line = false;
	} else {
		/*
			Workaround for fixing chat on Android. Lua doesn't handle
			the Cyrillic alphabet and some characters on older Android devices
		*/
#ifdef __ANDROID__
		line += L"<" + utf8_to_wide(name) + L"> " + wmessage;
#else
		line += utf8_to_wide(m_script->formatChatMessage(name,
				wide_to_utf8(wmessage)));
#endif
	}

	/*
		Tell calling method to send the message to sender
	*/
	if (!broadcast_line)
		return line;

	/*
		Send the message to others
	*/
	actionstream << "CHAT: " << wide_to_utf8(unescape_enriched(line)) << std::endl;

	ChatMessage chatmsg(line);

	if (!ServersNetworkObject->AreSlave) {
		std::vector<session_t> clients = m_clients.getClientIDs();
		for (u16 cid : clients)
			SendChatMessage(cid, chatmsg);
	} else {
		SendChatMessage(maskedu16(), chatmsg); //Putting 0 will make handle to all players
	}

	return L"";
}

void Server::handleAdminChat(const ChatEventChat *evt)
{
	std::string name = evt->nick;
	std::wstring wmessage = evt->evt_msg;

	std::wstring answer = handleChat(name, wmessage);

	// If asked to send answer to sender
	if (!answer.empty()) {
		m_admin_chat->outgoing_queue.push_back(new ChatEventChat("", answer));
	}
}

RemoteClient *Server::getClient(session_t peer_id, ClientState state_min)
{
	RemoteClient *client = getClientNoEx(peer_id,state_min);
	if(!client)
		throw ClientNotFoundException("Client not found");

	return client;
}
RemoteClient *Server::getClientNoEx(session_t peer_id, ClientState state_min)
{
	return m_clients.getClientNoEx(peer_id, state_min);
}

std::string Server::getPlayerName(session_t peer_id)
{
	RemotePlayer *player = m_env->getPlayer(peer_id);
	if (!player)
		return "[id="+itos(peer_id)+"]";
	return player->getName();
}

PlayerSAO *Server::getPlayerSAO(session_t peer_id)
{
	RemotePlayer *player = m_env->getPlayer(peer_id);
	if (!player)
		return NULL;
	return player->getPlayerSAO();
}

PlayerSAO *Server::getPlayerSAO(maskedu16 PID)
{
	RemotePlayer *player = m_env->getPlayer(PID);
	if (!player)
		return NULL;
	return player->getPlayerSAO();
}

std::string Server::getStatusString()
{
	std::ostringstream os(std::ios_base::binary);

	os << "# MineStars ShootingStars 2.5 <MultiCraft 2.0.6>: ";
	// Uptime
	os << ", uptime=" << m_uptime_counter->get();
	// Max lag estimate
	os << ", max_lag=" << (m_env ? m_env->getMaxLagEstimate() : 0);

	// Disabled due to misuse.
	// Information about clients
	/*bool first = true;
	os << ", clients={";
	if (m_env) {
		std::vector<session_t> clients = m_clients.getClientIDs();
		for (session_t client_id : clients) {
			RemotePlayer *player = m_env->getPlayer(client_id);

			// Get name of player
			const char *name = player ? player->getName() : "<unknown>";

			// Add name to information string
			if (!first)
				os << ", ";
			else
				first = false;
			os << name;
		}
	}
	os << "}";*/

	if (m_env && !((ServerMap*)(&m_env->getMap()))->isSavingEnabled())
		os << std::endl << "# Server: " << " WARNING: Map saving is disabled.";

	if (!g_settings->get("motd").empty())
		os << std::endl << "# Server: " << g_settings->get("motd");

	return os.str();
}

std::set<std::string> Server::getPlayerEffectivePrivs(const std::string &name)
{
	std::set<std::string> privs;
	m_script->getAuth(name, NULL, &privs);
	return privs;
}

bool Server::checkPriv(const std::string &name, const std::string &priv)
{
	std::set<std::string> privs = getPlayerEffectivePrivs(name);
	return (privs.count(priv) != 0);
}

void Server::reportPrivsModified(const std::string &name)
{
	if (!ServersNetworkObject->AreSlave) {
		if (name.empty()) {
			std::vector<session_t> clients = m_clients.getClientIDs();
			for (const session_t client_id : clients) {
				RemotePlayer *player = m_env->getPlayer(client_id);
				reportPrivsModified(player->getName());
			}
		} else {
			RemotePlayer *player = m_env->getPlayer(name.c_str());
			if (!player)
				return;
			SendPlayerPrivileges(player->getPeerId());
			PlayerSAO *sao = player->getPlayerSAO();
			if(!sao)
				return;
			sao->updatePrivileges(
					getPlayerEffectivePrivs(name),
					isSingleplayer());
		}
	} else {
		if (name.empty()) {
			for (auto it = ClientDataTable.begin(); it != ClientDataTable.end(); ++it) {
				RemotePlayer *player = m_env->getPlayer(maskedu16(it->first));
				reportPrivsModified(player->getName());
			}
		} else {
			RemotePlayer *player = m_env->getPlayer(name.c_str());
			if (!player)
				return;
			SendPlayerPrivileges(maskedu16(player->player_id));
			PlayerSAO *sao = player->getPlayerSAO();
			if(!sao)
				return;
			sao->updatePrivileges(getPlayerEffectivePrivs(name), isSingleplayer());
		}
	}
}

void Server::reportInventoryFormspecModified(const std::string &name)
{
	RemotePlayer *player = m_env->getPlayer(name.c_str());
	if (!player)
		return;
	if (!ServersNetworkObject->AreSlave)
		SendPlayerInventoryFormspec(player->getPeerId());
	else {
		//player->player_id
		maskedu16 val = maskedu16(player->player_id);
		SendPlayerInventoryFormspec(val);
	}
}

void Server::reportFormspecPrependModified(const std::string &name)
{
	RemotePlayer *player = m_env->getPlayer(name.c_str());
	if (!player)
		return;
	if (!ServersNetworkObject->AreSlave)
		SendPlayerFormspecPrepend(player->getPeerId());
	else
		SendPlayerFormspecPrepend(maskedu16(player->player_id));
}

void Server::setIpBanned(const std::string &ip, const std::string &name)
{
	m_banmanager->add(ip, name);
}

void Server::unsetIpBanned(const std::string &ip_or_name)
{
	m_banmanager->remove(ip_or_name);
}

std::string Server::getBanDescription(const std::string &ip_or_name)
{
	return m_banmanager->getBanDescription(ip_or_name);
}

void Server::notifyPlayer(const char *name, const std::wstring &msg)
{
	// m_env will be NULL if the server is initializing
	if (!m_env)
		return;

	if (m_admin_nick == name && !m_admin_nick.empty()) {
		m_admin_chat->outgoing_queue.push_back(new ChatEventChat("", msg));
	}

	RemotePlayer *player = m_env->getPlayer(name);
	if (!player) {
		return;
	}

	if (!ServersNetworkObject->AreSlave) {
		if (player->getPeerId() == PEER_ID_INEXISTENT)
			return;
		SendChatMessage(player->getPeerId(), ChatMessage(msg));
	} else {
		if (player->player_id == 0)
			return;
		maskedu16 n = maskedu16(player->player_id);
		SendChatMessage(n, ChatMessage(msg));
	}
}

bool Server::showFormspec(const char *playername, const std::string &formspec, const std::string &formname)
{
	// m_env will be NULL if the server is initializing
	if (!m_env)
		return false;

	RemotePlayer *player = m_env->getPlayer(playername);
	if (!player)
		return false;

	if (!ServersNetworkObject->AreSlave)
		SendShowFormspecMessage(player->getPeerId(), formspec, formname);
	else {
		maskedu16 n = maskedu16(player->player_id);
		SendShowFormspecMessage(n, formspec, formname);
	}
	return true;
}

u32 Server::hudAdd(RemotePlayer *player, HudElement *form)
{
	if (!player)
		return -1;

	u32 id = player->addHud(form);

	if (!ServersNetworkObject->AreSlave)
		SendHUDAdd(player->getPeerId(), id, form);
	else {
		maskedu16 n = maskedu16(player->player_id);
		SendHUDAdd(n, id, form);
	}
	return id;
}

bool Server::hudRemove(RemotePlayer *player, u32 id) {
	if (!player)
		return false;

	HudElement* todel = player->removeHud(id);

	if (!todel)
		return false;

	delete todel;

	if (!ServersNetworkObject->AreSlave)
		SendHUDRemove(player->getPeerId(), id);
	else {
		maskedu16 n = maskedu16(player->player_id);
		SendHUDRemove(n, id);
	}
	return true;
}

bool Server::hudChange(RemotePlayer *player, u32 id, HudElementStat stat, void *data)
{
	if (!player)
		return false;

	if (!ServersNetworkObject->AreSlave)
		SendHUDChange(player->getPeerId(), id, stat, data);
	else {
		maskedu16 n = maskedu16(player->player_id);
		SendHUDChange(n, id, stat, data);
	}
	return true;
}

bool Server::hudSetFlags(RemotePlayer *player, u32 flags, u32 mask)
{
	if (!player)
		return false;

	if (!ServersNetworkObject->AreSlave)
		SendHUDSetFlags(player->getPeerId(), flags, mask);
	else {
		maskedu16 n = maskedu16(player->player_id);
		SendHUDSetFlags(n, flags, mask);
	}
	player->hud_flags &= ~mask;
	player->hud_flags |= flags;

	PlayerSAO* playersao = player->getPlayerSAO();

	if (!playersao)
		return false;

	m_script->player_event(playersao, "hud_changed");
	return true;
}

bool Server::hudSetHotbarItemcount(RemotePlayer *player, s32 hotbar_itemcount)
{
	if (!player)
		return false;

	if (hotbar_itemcount <= 0 || hotbar_itemcount > HUD_HOTBAR_ITEMCOUNT_MAX)
		return false;

	player->setHotbarItemcount(hotbar_itemcount);
	std::ostringstream os(std::ios::binary);
	writeS32(os, hotbar_itemcount);
	if (!ServersNetworkObject->AreSlave)
		SendHUDSetParam(player->getPeerId(), HUD_PARAM_HOTBAR_ITEMCOUNT, os.str());
	else {
		maskedu16 n = maskedu16(player->player_id);
		SendHUDSetParam(n, HUD_PARAM_HOTBAR_ITEMCOUNT, os.str());
	}
	return true;
}

void Server::hudSetHotbarImage(RemotePlayer *player, const std::string &name)
{
	if (!player)
		return;

	player->setHotbarImage(name);
	if (!ServersNetworkObject->AreSlave)
		SendHUDSetParam(player->getPeerId(), HUD_PARAM_HOTBAR_IMAGE, name);
	else {
		maskedu16 n = maskedu16(player->player_id);
		SendHUDSetParam(n, HUD_PARAM_HOTBAR_IMAGE, name);
	}
}

void Server::hudSetHotbarSelectedImage(RemotePlayer *player, const std::string &name)
{
	if (!player)
		return;

	player->setHotbarSelectedImage(name);
	if (!ServersNetworkObject->AreSlave)
		SendHUDSetParam(player->getPeerId(), HUD_PARAM_HOTBAR_SELECTED_IMAGE, name);
	else {
		maskedu16 n = maskedu16(player->player_id);
		SendHUDSetParam(n, HUD_PARAM_HOTBAR_SELECTED_IMAGE, name);
	}
}

Address Server::getPeerAddress(session_t peer_id)
{
	// Note that this is only set after Init was received in Server::handleCommand_Init
	return getClient(peer_id, CS_Invalid)->getAddress();
}

void Server::setLocalPlayerAnimations(RemotePlayer *player,
		v2s32 animation_frames[4], f32 frame_speed)
{
	sanity_check(player);
	player->setLocalAnimations(animation_frames, frame_speed);
	if (!ServersNetworkObject->AreSlave)
		SendLocalPlayerAnimations(player->getPeerId(), animation_frames, frame_speed);
	else
		SendLocalPlayerAnimations(maskedu16(player->player_id), animation_frames, frame_speed);
}

void Server::setPlayerEyeOffset(RemotePlayer *player, const v3f &first, const v3f &third)
{
	sanity_check(player);
	player->eye_offset_first = first;
	player->eye_offset_third = third;
	if (!ServersNetworkObject->AreSlave)
		SendEyeOffset(player->getPeerId(), first, third);
	else
		SendEyeOffset(maskedu16(player->player_id), first, third);
}

void Server::setSky(RemotePlayer *player, const SkyboxParams &params)
{
	sanity_check(player);
	player->setSky(params);
	if (!ServersNetworkObject->AreSlave)
		SendSetSky(player->getPeerId(), params);
	else
		SendSetSky(maskedu16(player->player_id), params);
}

void Server::setSun(RemotePlayer *player, const SunParams &params)
{
	sanity_check(player);
	player->setSun(params);
	if (!ServersNetworkObject->AreSlave)
		SendSetSun(player->getPeerId(), params);
	else
		SendSetSun(maskedu16(player->player_id), params);
}

void Server::setMoon(RemotePlayer *player, const MoonParams &params)
{
	sanity_check(player);
	player->setMoon(params);
	if (!ServersNetworkObject->AreSlave)
		SendSetMoon(player->getPeerId(), params);
	else
		SendSetMoon(maskedu16(player->player_id), params);
}

void Server::setStars(RemotePlayer *player, const StarParams &params)
{
	sanity_check(player);
	player->setStars(params);
	if (!ServersNetworkObject->AreSlave)
		SendSetStars(player->getPeerId(), params);
	else
		SendSetStars(maskedu16(player->player_id), params);
}

void Server::setClouds(RemotePlayer *player, const CloudParams &params)
{
	sanity_check(player);
	player->setCloudParams(params);
	if (!ServersNetworkObject->AreSlave)
		SendCloudParams(player->getPeerId(), params);
	else
		SendCloudParams(maskedu16(player->player_id), params);
}

void Server::overrideDayNightRatio(RemotePlayer *player, bool do_override,
	float ratio)
{
	sanity_check(player);
	player->overrideDayNightRatio(do_override, ratio);
	if (!ServersNetworkObject->AreSlave)
		SendOverrideDayNightRatio(player->getPeerId(), do_override, ratio);
	else
		SendOverrideDayNightRatio(maskedu16(player->player_id), do_override, ratio);
}

void Server::notifyPlayers(const std::wstring &msg)
{
	if (!ServersNetworkObject->AreSlave)
		SendChatMessage(PEER_ID_INEXISTENT, ChatMessage(msg));
	else
		SendChatMessage(maskedu16(), ChatMessage(msg));
}

void Server::spawnParticle(const std::string &playername,
	const ParticleParameters &p)
{
	// m_env will be NULL if the server is initializing
	if (!m_env)
		return;

	session_t peer_id = PEER_ID_INEXISTENT;
	u16 proto_ver = 0;
	u16 playerid = 0;
	
	if (!ServersNetworkObject->AreSlave) {
		if (!playername.empty()) {
			RemotePlayer *player = m_env->getPlayer(playername.c_str());
			if (!player)
				return;
			peer_id = player->getPeerId();
			proto_ver = player->protocol_version;
		}
		SendSpawnParticle(peer_id, proto_ver, p);
	} else {
		if (!playername.empty()) {
			RemotePlayer *player = m_env->getPlayer(playername.c_str());
			if (!player)
				return;
			playerid = player->player_id;
			proto_ver = player->protocol_version;
		}
		SendSpawnParticle(maskedu16(playerid), proto_ver, p);
	}
}

u32 Server::addParticleSpawner(const ParticleSpawnerParameters &p,
	ServerActiveObject *attached, const std::string &playername)
{
	// m_env will be NULL if the server is initializing
	if (!m_env)
		return -1;

	session_t peer_id = PEER_ID_INEXISTENT;
	u16 proto_ver = 0;
	u16 playerid = 0;
	u16 attached_id = attached ? attached->getId() : 0;
	u32 id;
	if (!ServersNetworkObject->AreSlave) {
		if (!playername.empty()) {
			RemotePlayer *player = m_env->getPlayer(playername.c_str());
			if (!player)
				return -1;
			peer_id = player->getPeerId();
			proto_ver = player->protocol_version;
		}
		if (attached_id == 0)
			id = m_env->addParticleSpawner(p.time);
		else
			id = m_env->addParticleSpawner(p.time, attached_id);
		SendAddParticleSpawner(peer_id, proto_ver, p, attached_id, id);
	} else {
		if (!playername.empty()) {
			RemotePlayer *player = m_env->getPlayer(playername.c_str());
			if (!player)
				return -1;
			proto_ver = player->protocol_version;
			playerid = player->getPlayerID();
		}
		if (attached_id == 0)
			id = m_env->addParticleSpawner(p.time);
		else
			id = m_env->addParticleSpawner(p.time, attached_id);
		SendAddParticleSpawner(maskedu16(playerid), proto_ver, p, attached_id, id);
	}
	return id;
}

void Server::deleteParticleSpawner(const std::string &playername, u32 id)
{
	// m_env will be NULL if the server is initializing
	if (!m_env)
		throw ServerError("Can't delete particle spawners during initialisation!");

	session_t peer_id = PEER_ID_INEXISTENT;
	u16 playerid = 0; //default
	
	if (!ServersNetworkObject->AreSlave) {
		if (!playername.empty()) {
			RemotePlayer *player = m_env->getPlayer(playername.c_str());
			if (!player)
				return;
			peer_id = player->getPeerId();
		}
		SendDeleteParticleSpawner(peer_id, id);
	} else { //^-^
		if (!playername.empty()) { RemotePlayer *player = m_env->getPlayer(playername.c_str()); if (!player) return; playerid = player->getPlayerID(); SendDeleteParticleSpawner(maskedu16(playerid), id); }
	}
	m_env->deleteParticleSpawner(id);
}

bool Server::dynamicAddMedia(const std::string &filepath,
	std::vector<RemotePlayer*> &sent_to)
{
	if (ServersNetworkObject->AreSlave) {
		errorstream << "Illegal use of 'dynamicAddMedia' on a Slave server, returning." << std::endl;
		return false;
	}
	std::string filename = fs::GetFilenameFromPath(filepath.c_str());
	if (m_media.find(filename) != m_media.end()) {
		errorstream << "Server::dynamicAddMedia(): file \"" << filename
			<< "\" already exists in media cache" << std::endl;
		return false;
	}

	// Load the file and add it to our media cache
	std::string filedata, raw_hash;
	bool ok = addMediaFile(filename, filepath, &filedata, &raw_hash);
	if (!ok)
		return false;

	// Push file to existing clients
	NetworkPacket pkt(TOCLIENT_MEDIA_PUSH, 0);
	pkt << raw_hash << filename << (bool) true;
	pkt.putLongString(filedata);

	m_clients.lock();
	for (auto &pair : m_clients.getClientList()) {
		if (pair.second->getState() < CS_DefinitionsSent)
			continue;
		if (pair.second->net_proto_version < 39)
			continue;

		if (auto player = m_env->getPlayer(pair.second->peer_id))
			sent_to.emplace_back(player);
		/*
			FIXME: this is a very awful hack
			The network layer only guarantees ordered delivery inside a channel.
			Since the very next packet could be one that uses the media, we have
			to push the media over ALL channels to ensure it is processed before
			it is used.
			In practice this means we have to send it twice:
			- channel 1 (HUD)
			- channel 0 (everything else: e.g. play_sound, object messages)
		*/
		m_clients.send(pair.second->peer_id, 1, &pkt, true);
		m_clients.send(pair.second->peer_id, 0, &pkt, true);
	}
	m_clients.unlock();

	return true;
}

// actions: time-reversed list
// Return value: success/failure
#if USE_SQLITE
bool Server::rollbackRevertActions(const std::list<RollbackAction> &actions,
		std::list<std::string> *log)
{
	infostream<<"Server::rollbackRevertActions(len="<<actions.size()<<")"<<std::endl;
	ServerMap *map = (ServerMap*)(&m_env->getMap());

	// Fail if no actions to handle
	if (actions.empty()) {
		assert(log);
		log->push_back("Nothing to do.");
		return false;
	}

	int num_tried = 0;
	int num_failed = 0;

	for (const RollbackAction &action : actions) {
		num_tried++;
		bool success = action.applyRevert(map, m_inventory_mgr.get(), this);
		if(!success){
			num_failed++;
			std::ostringstream os;
			os<<"Revert of step ("<<num_tried<<") "<<action.toString()<<" failed";
			infostream<<"Map::rollbackRevertActions(): "<<os.str()<<std::endl;
			if (log)
				log->push_back(os.str());
		}else{
			std::ostringstream os;
			os<<"Successfully reverted step ("<<num_tried<<") "<<action.toString();
			infostream<<"Map::rollbackRevertActions(): "<<os.str()<<std::endl;
			if (log)
				log->push_back(os.str());
		}
	}

	infostream<<"Map::rollbackRevertActions(): "<<num_failed<<"/"<<num_tried
			<<" failed"<<std::endl;

	// Call it done if less than half failed
	return num_failed <= num_tried/2;
}
#endif

// IGameDef interface
// Under envlock
IItemDefManager *Server::getItemDefManager()
{
	return m_itemdef;
}

const NodeDefManager *Server::getNodeDefManager()
{
	return m_nodedef;
}

ICraftDefManager *Server::getCraftDefManager()
{
	return m_craftdef;
}

u16 Server::allocateUnknownNodeId(const std::string &name)
{
	return m_nodedef->allocateDummy(name);
}

IWritableItemDefManager *Server::getWritableItemDefManager()
{
	return m_itemdef;
}

NodeDefManager *Server::getWritableNodeDefManager()
{
	return m_nodedef;
}

IWritableCraftDefManager *Server::getWritableCraftDefManager()
{
	return m_craftdef;
}

const std::vector<ModSpec> & Server::getMods() const
{
	return m_modmgr->getMods();
}

const ModSpec *Server::getModSpec(const std::string &modname) const
{
	return m_modmgr->getModSpec(modname);
}

void Server::getModNames(std::vector<std::string> &modlist)
{
	m_modmgr->getModNames(modlist);
}

std::string Server::getBuiltinLuaPath()
{
	return porting::path_share + DIR_DELIM + "builtin";
}

std::string Server::getModStoragePath() const
{
	return m_path_world + DIR_DELIM + "mod_storage";
}

v3f Server::findSpawnPos()
{
	ServerMap &map = m_env->getServerMap();
	v3f nodeposf;
	if (g_settings->getV3FNoEx("static_spawnpoint", nodeposf) ||
			m_env->getWorldSpawnpoint(nodeposf))
		return nodeposf * BS;

	bool is_good = false;
	// Limit spawn range to mapgen edges (determined by 'mapgen_limit')
	s32 range_max = map.getMapgenParams()->getSpawnRangeMax();

	// Try to find a good place a few times
	for (s32 i = 0; i < 4000 && !is_good; i++) {
		s32 range = MYMIN(1 + i, range_max);
		// We're going to try to throw the player to this position
		v2s16 nodepos2d = v2s16(
			-range + (myrand() % (range * 2)),
			-range + (myrand() % (range * 2)));
		// Get spawn level at point
		s16 spawn_level = m_emerge->getSpawnLevelAtPoint(nodepos2d);
		// Continue if MAX_MAP_GENERATION_LIMIT was returned by the mapgen to
		// signify an unsuitable spawn position, or if outside limits.
		if (spawn_level >= MAX_MAP_GENERATION_LIMIT ||
				spawn_level <= -MAX_MAP_GENERATION_LIMIT)
			continue;

		v3s16 nodepos(nodepos2d.X, spawn_level, nodepos2d.Y);
		// Consecutive empty nodes
		s32 air_count = 0;

		// Search upwards from 'spawn level' for 2 consecutive empty nodes, to
		// avoid obstructions in already-generated mapblocks.
		// In ungenerated mapblocks consisting of 'ignore' nodes, there will be
		// no obstructions, but mapgen decorations are generated after spawn so
		// the player may end up inside one.
		for (s32 i = 0; i < 8; i++) {
			v3s16 blockpos = getNodeBlockPos(nodepos);
			map.emergeBlock(blockpos, true);
			content_t c = map.getNode(nodepos).getContent();

			// In generated mapblocks allow spawn in all 'airlike' drawtype nodes.
			// In ungenerated mapblocks allow spawn in 'ignore' nodes.
			if (m_nodedef->get(c).drawtype == NDT_AIRLIKE || c == CONTENT_IGNORE) {
				air_count++;
				if (air_count >= 2) {
					// Spawn in lower empty node
					nodepos.Y--;
					nodeposf = intToFloat(nodepos, BS);
					// Don't spawn the player outside map boundaries
					if (objectpos_over_limit(nodeposf))
						// Exit this loop, positions above are probably over limit
						break;

					// Good position found, cause an exit from main loop
					is_good = true;
					break;
				}
			} else {
				air_count = 0;
			}
			nodepos.Y++;
		}
	}

	if (is_good)
		return nodeposf;

	// No suitable spawn point found, return fallback 0,0,0
	return v3f(0.0f, 0.0f, 0.0f);
}

void Server::requestShutdown(const std::string &msg, bool reconnect, float delay)
{
	if (delay == 0.0f) {
	// No delay, shutdown immediately
		m_shutdown_state.is_requested = true;
		// only print to the infostream, a chat message saying
		// "Server Shutting Down" is sent when the server destructs.
		infostream << "*** Immediate Server shutdown requested." << std::endl;
	} else if (delay < 0.0f && m_shutdown_state.isTimerRunning()) {
		// Negative delay, cancel shutdown if requested
		m_shutdown_state.reset();
		std::wstringstream ws;

		ws << L"*** Server shutdown canceled.";

		infostream << wide_to_utf8(ws.str()).c_str() << std::endl;
		if (!ServersNetworkObject->AreSlave)
			SendChatMessage(PEER_ID_INEXISTENT, ws.str());
		else
			SendChatMessage(maskedu16(), ws.str());
		// m_shutdown_* are already handled, skip.
		return;
	} else if (delay > 0.0f) {
	// Positive delay, tell the clients when the server will shut down
		std::wstringstream ws;

		ws << L"*** Server shutting down in "
				<< duration_to_string(myround(delay)).c_str()
				<< ".";

		infostream << wide_to_utf8(ws.str()).c_str() << std::endl;
		if (!ServersNetworkObject->AreSlave)
			SendChatMessage(PEER_ID_INEXISTENT, ws.str());
		else
			SendChatMessage(maskedu16(), ws.str());
	}

	m_shutdown_state.trigger(delay, msg, reconnect);
}

bool Server::ExistsID(u16 ID) {
	//return !ActivePlayersU16[ID];
	bool got = false;
	auto it = ActivePlayersU16.find(ID);
	if (it != ActivePlayersU16.end()) {
		got = true;
	}
	return got;
}

u16 Server::GetRandomIDforPlayer() {
	static std::random_device rd;
	static std::mt19937 gen(rd());
	std::uniform_int_distribution<u16> dist(0, UINT16_MAX);
	return dist(gen);
}

PlayerSAO* Server::emergePlayer(const char *name, session_t peer_id, u16 proto_version)
{
	/*
		Try to get an existing player
	*/
	RemotePlayer *player = m_env->getPlayer(name);

	// If player is already connected, cancel
	if (player && player->getPeerId() != PEER_ID_INEXISTENT) {
		infostream<<"emergePlayer(): Player already connected"<<std::endl;
		return NULL;
	}

	/*
		If player with the wanted peer_id already exists, cancel.
	*/
	if (m_env->getPlayer(peer_id)) {
		warningstream << "emergePlayer(): Player with wrong name but same peer_id already exists" << std::endl;
		return NULL;
	}

	if (!player) {
		player = new RemotePlayer(name, idef());
	}

	bool newplayer = false;

	u16 preferID;
	bool got_id = false;
	while (!got_id) {
		u16 to_use_rdm = GetRandomIDforPlayer();
		if (!ExistsID(to_use_rdm)) {
			got_id = true;
			preferID = to_use_rdm;
		}
	}
	
	actionstream << "Player ID for " << preferID << " got!" << std::endl;

	player->player_id = preferID; //set Player ID

	//Set some settings for ez coding
	SessionsToU16[peer_id] = preferID;
	SessionsStatus[peer_id] = false;
	PlayerInEnvironmentToServer[peer_id] = (u16)0;
	ActivePlayersU16[preferID] = true;

	// Load player
	PlayerSAO *playersao = m_env->loadPlayer(player, &newplayer, peer_id, isSingleplayer());

	

	// Complete init with server parts
	playersao->finalize(player, getPlayerEffectivePrivs(player->getName()));
	player->protocol_version = proto_version;

	/* Run scripts */
	if (newplayer) {
		m_script->on_newplayer(playersao);
	}

	return playersao;
}

/*
	{ "TOSERVER_GOT_DISCONNECT",           TOSERVER_STATE_INGAME, &Server::handleCommand_GotDisconnect }, // 0x53
	{ "TOSERVER_GOT_CONNECT",              TOSERVER_STATE_INGAME, &Server::handleCommand_GotConnect }, // 0x54
	{ "TOSERVER_SAY_HI",                   TOSERVER_STATE_INGAME, &Server::handleCommand_SayHi }, // 0x55
	{ "TOSERVER_HI",                       TOSERVER_STATE_INGAME, &Server::handleCommand_Hi }, // 0x56
	
	pkt << player_id;
	pkt << player->getName();
	pkt << player->proto_min;
	pkt << player->proto_max;
	pkt << player->protocol_version;
	if (usingdat) {
		pkt << info.vers_string.c_str();
		pkt << info.platform.c_str();
	}
*/

//Only applicable when this is an slave server
PlayerSAO* Server::emergePlayer(const char *name, maskedu16 _pid, u16 proto_version) {
	u16 pid = _pid.value;
	
	RemotePlayer *player = new RemotePlayer(name, idef());
	
	if (!player) {
		errorstream << "Crash is coming" << std::endl;
	}
	
	player->player_id = pid;
	
	bool newplayer = false;
	// Load player
	PlayerSAO *playersao = m_env->loadPlayer(player, &newplayer, _pid, isSingleplayer());
	playersao->setServerState(true);
	playersao->finalize(player, getPlayerEffectivePrivs(player->getName()));
	player->protocol_version = proto_version;
	
	NetworkPacket _pkt(0x76, 5);
	_pkt << (u8)ServersNetworkObject->QueryThisServerID();
	_pkt << pid << playersao->getId();
	Send(&_pkt);

	//new player => m_script
	if (newplayer) {
		m_script->on_newplayer(playersao);
	}

	return playersao;
}

bool Server::registerModStorage(ModMetadata *storage)
{
	if (m_mod_storages.find(storage->getModName()) != m_mod_storages.end()) {
		errorstream << "Unable to register same mod storage twice. Storage name: "
				<< storage->getModName() << std::endl;
		return false;
	}

	m_mod_storages[storage->getModName()] = storage;
	return true;
}

void Server::unregisterModStorage(const std::string &name)
{
	std::unordered_map<std::string, ModMetadata *>::const_iterator it = m_mod_storages.find(name);
	if (it != m_mod_storages.end()) {
		// Save unconditionaly on unregistration
		it->second->save(getModStoragePath());
		m_mod_storages.erase(name);
	}
}

void dedicated_server_loop(Server &server, bool &kill)
{
	verbosestream<<"dedicated_server_loop()"<<std::endl;

	IntervalLimiter m_profiler_interval;

	static thread_local const float steplen =
			0.02;
	static thread_local const float profiler_print_interval =
			g_settings->getFloat("profiler_print_interval");

	/*
	 * The dedicated server loop only does time-keeping (in Server::step) and
	 * provides a way to main.cpp to kill the server externally (bool &kill).
	 */

	for(;;) {
		// This is kind of a hack but can be done like this
		// because server.step() is very light
		sleep_ms((int)(steplen*1000.0));
		server.step(steplen);

		if (server.isShutdownRequested() || kill)
			break;

		/*
			Profiler
		*/
		if (profiler_print_interval != 0) {
			if(m_profiler_interval.step(steplen, profiler_print_interval))
			{
				infostream<<"Profiler:"<<std::endl;
				g_profiler->print(infostream);
				g_profiler->clear();
			}
		}
	}

	infostream << "Dedicated server quitting" << std::endl;
#if USE_CURL
	if (g_settings->getBool("server_announce"))
		ServerList::sendAnnounce(ServerList::AA_DELETE,
			server.m_bind_addr.getPort());
#endif
}

/*
 * Mod channels
 */


bool Server::joinModChannel(const std::string &channel)
{
	if (ServersNetworkObject->AreSlave) {
		errorstream << "Unsupported joinModChannel on SlaveServer for: " << channel << std::endl;
		return false;
	}
	return m_modchannel_mgr->joinChannel(channel, PEER_ID_SERVER) && m_modchannel_mgr->setChannelState(channel, MODCHANNEL_STATE_READ_WRITE);
}

bool Server::leaveModChannel(const std::string &channel)
{
	if (ServersNetworkObject->AreSlave) {
		errorstream << "Unsupported leaveModChannel on SlaveServer for: " << channel << std::endl;
		return false;
	}
	return m_modchannel_mgr->leaveChannel(channel, PEER_ID_SERVER);
}

bool Server::sendModChannelMessage(const std::string &channel, const std::string &message)
{
	if (ServersNetworkObject->AreSlave) {
		errorstream << "Unsupported sendModChannelMessage on SlaveServer for: " << channel << std::endl;
		return false;
	}
	if (!m_modchannel_mgr->canWriteOnChannel(channel))
		return false;

	broadcastModChannelMessage(channel, message, PEER_ID_SERVER);
	return true;
}

ModChannel* Server::getModChannel(const std::string &channel)
{
	if (ServersNetworkObject->AreSlave) {
		errorstream << "Unsupported getModChannel on SlaveServer for: " << channel << std::endl;
		return nullptr;
	}
	return m_modchannel_mgr->getModChannel(channel);
}

void Server::broadcastModChannelMessage(const std::string &channel,
		const std::string &message, session_t from_peer)
{
	if (ServersNetworkObject->AreSlave) {
		errorstream << "Unsupported broadcastModChannelMessage on SlaveServer for: " << channel << std::endl;
		return;
	}
	const std::vector<u16> &peers = m_modchannel_mgr->getChannelPeers(channel);
	if (peers.empty())
		return;

	if (message.size() > STRING_MAX_LEN) {
		warningstream << "ModChannel message too long, dropping before sending "
				<< " (" << message.size() << " > " << STRING_MAX_LEN << ", channel: "
				<< channel << ")" << std::endl;
		return;
	}

	std::string sender;
	if (from_peer != PEER_ID_SERVER) {
		sender = getPlayerName(from_peer);
	}

	NetworkPacket resp_pkt(TOCLIENT_MODCHANNEL_MSG,
			2 + channel.size() + 2 + sender.size() + 2 + message.size());
	resp_pkt << channel << sender << message;
	for (session_t peer_id : peers) {
		// Ignore sender
		if (peer_id == from_peer)
			continue;

		Send(peer_id, &resp_pkt);
	}

	if (from_peer != PEER_ID_SERVER) {
		m_script->on_modchannel_message(channel, sender, message);
	}
}

Translations *Server::getTranslationLanguage(const std::string &lang_code)
{
	if (lang_code.empty())
		return nullptr;

	auto it = server_translations.find(lang_code);
	if (it != server_translations.end())
		return &it->second; // Already loaded

	// [] will create an entry
	auto *translations = &server_translations[lang_code];

	std::string suffix = "." + lang_code + ".tr";
	for (const auto &i : m_media) {
		if (str_ends_with(i.first, suffix)) {
			std::string data;
			if (fs::ReadFile(i.second.path, data)) {
				translations->loadTranslation(data);
			}
		}
	}

	return translations;
}
