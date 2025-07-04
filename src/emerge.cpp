/*
Minetest
Copyright (C) 2010-2013 celeron55, Perttu Ahola <celeron55@gmail.com>
Copyright (C) 2010-2013 kwolekr, Ryan Kwolek <kwolekr@minetest.net>

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


#include "emerge_internal.h"

#include <iostream>
#include <queue>

#include "util/container.h"
#include "util/thread.h"
#include "threading/event.h"

#include "config.h"
#include "constants.h"
#include "environment.h"
#include "log.h"
#include "map.h"
#include "mapblock.h"
#include "mapgen/mg_biome.h"
#include "mapgen/mg_ore.h"
#include "mapgen/mg_decoration.h"
#include "mapgen/mg_schematic.h"
#include "nodedef.h"
#include "profiler.h"
#include "scripting_server.h"
#include "scripting_emerge.h"
#include "server.h"
#include "settings.h"
#include "voxel.h"

EmergeParams::~EmergeParams()
{
	infostream << "EmergeParams: destroying " << this << std::endl;
	// Delete everything that was cloned on creation of EmergeParams
	delete biomegen;
	delete biomemgr;
	delete oremgr;
	delete decomgr;
	delete schemmgr;
}

EmergeParams::EmergeParams(EmergeManager *parent, const BiomeGen *biomegen,
	const BiomeManager *biomemgr,
	const OreManager *oremgr, const DecorationManager *decomgr,
	const SchematicManager *schemmgr) :
	ndef(parent->ndef),
	enable_mapgen_debug_info(parent->enable_mapgen_debug_info),
	gen_notify_on(parent->gen_notify_on),
	gen_notify_on_deco_ids(&parent->gen_notify_on_deco_ids),
	gen_notify_on_custom(&parent->gen_notify_on_custom),
	biomemgr(biomemgr->clone()), oremgr(oremgr->clone()),
	decomgr(decomgr->clone()), schemmgr(schemmgr->clone())
{
	this->biomegen = biomegen->clone(this->biomemgr);
}

////
//// EmergeManager
////

EmergeManager::EmergeManager(Server *server)
{
	this->ndef      = server->getNodeDefManager();
	this->biomemgr  = new BiomeManager(server);
	this->oremgr    = new OreManager(server);
	this->decomgr   = new DecorationManager(server);
	this->schemmgr  = new SchematicManager(server);

	this->biomegen = nullptr;

	// Note that accesses to this variable are not synchronized.
	// This is because the *only* thread ever starting or stopping
	// EmergeThreads should be the ServerThread.

	enable_mapgen_debug_info = g_settings->getBool("enable_mapgen_debug_info");

	s16 nthreads = 1;
	g_settings->getS16NoEx("num_emerge_threads", nthreads);
	// If automatic, leave a proc for the main thread and one for
	// some other misc thread
	if (nthreads == 0)
		nthreads = Thread::getNumberOfProcessors() - 2;
	if (nthreads < 1)
		nthreads = 1;

	m_qlimit_total = g_settings->getU16("emergequeue_limit_total");
	// FIXME: these fallback values are probably not good
	if (!g_settings->getU16NoEx("emergequeue_limit_diskonly", m_qlimit_diskonly))
		m_qlimit_diskonly = nthreads * 5 + 1;
	if (!g_settings->getU16NoEx("emergequeue_limit_generate", m_qlimit_generate))
		m_qlimit_generate = nthreads + 1;

	// don't trust user input for something very important like this
	if (m_qlimit_total < 1)
		m_qlimit_total = 1;
	if (m_qlimit_diskonly < 1)
		m_qlimit_diskonly = 1;
	if (m_qlimit_generate < 1)
		m_qlimit_generate = 1;

	for (s16 i = 0; i < nthreads; i++)
		m_threads.push_back(new EmergeThread(server, i));

	infostream << "EmergeManager: using " << nthreads << " threads" << std::endl;
}


EmergeManager::~EmergeManager()
{
	for (u32 i = 0; i != m_threads.size(); i++) {
		EmergeThread *thread = m_threads[i];

		if (m_threads_active) {
			thread->stop();
			thread->signal();
			thread->wait();
		}

		delete thread;

		// Mapgen init might not be finished if there is an error during startup.
		if (m_mapgens.size() > i)
			delete m_mapgens[i];
	}

	delete biomegen;
	delete biomemgr;
	delete oremgr;
	delete decomgr;
	delete schemmgr;
}


BiomeManager *EmergeManager::getWritableBiomeManager()
{
	FATAL_ERROR_IF(!m_mapgens.empty(),
		"Writable managers can only be returned before mapgen init");
	return biomemgr;
}

OreManager *EmergeManager::getWritableOreManager()
{
	FATAL_ERROR_IF(!m_mapgens.empty(),
		"Writable managers can only be returned before mapgen init");
	return oremgr;
}

DecorationManager *EmergeManager::getWritableDecorationManager()
{
	FATAL_ERROR_IF(!m_mapgens.empty(),
		"Writable managers can only be returned before mapgen init");
	return decomgr;
}

SchematicManager *EmergeManager::getWritableSchematicManager()
{
	FATAL_ERROR_IF(!m_mapgens.empty(),
		"Writable managers can only be returned before mapgen init");
	return schemmgr;
}


void EmergeManager::initMapgens(MapgenParams *params)
{
	FATAL_ERROR_IF(!m_mapgens.empty(), "Mapgen already initialised.");

	v3s16 csize = v3s16(1, 1, 1) * (params->chunksize * MAP_BLOCKSIZE);
	biomegen = biomemgr->createBiomeGen(BIOMEGEN_ORIGINAL, params->bparams, csize);

	mgparams = params;

	for (u32 i = 0; i != m_threads.size(); i++) {
		EmergeParams *p = new EmergeParams(
			this, biomegen, biomemgr, oremgr, decomgr, schemmgr);
		infostream << "EmergeManager: Created params " << p
			<< " for thread " << i << std::endl;
		m_mapgens.push_back(Mapgen::createMapgen(params->mgtype, params, p));
	}
}


Mapgen *EmergeManager::getCurrentMapgen()
{
	if (!m_threads_active)
		return nullptr;

	for (u32 i = 0; i != m_threads.size(); i++) {
		EmergeThread *t = m_threads[i];
		if (t->isRunning() && t->isCurrentThread())
			return t->m_mapgen;
	}

	return nullptr;
}


void EmergeManager::startThreads()
{
	if (m_threads_active)
		return;

	for (u32 i = 0; i != m_threads.size(); i++)
		m_threads[i]->start();

	m_threads_active = true;
}


void EmergeManager::stopThreads()
{
	if (!m_threads_active)
		return;

	// Request thread stop in parallel
	for (u32 i = 0; i != m_threads.size(); i++) {
		m_threads[i]->stop();
		m_threads[i]->signal();
	}

	// Then do the waiting for each
	for (u32 i = 0; i != m_threads.size(); i++)
		m_threads[i]->wait();

	m_threads_active = false;
}


bool EmergeManager::isRunning()
{
	return m_threads_active;
}


bool EmergeManager::enqueueBlockEmerge(
	session_t peer_id,
	v3s16 blockpos,
	bool allow_generate,
	bool ignore_queue_limits)
{
	u16 flags = 0;
	if (allow_generate)
		flags |= BLOCK_EMERGE_ALLOW_GEN;
	if (ignore_queue_limits)
		flags |= BLOCK_EMERGE_FORCE_QUEUE;

	return enqueueBlockEmergeEx(blockpos, peer_id, flags, NULL, NULL);
}


bool EmergeManager::enqueueBlockEmergeEx(
	v3s16 blockpos,
	session_t peer_id,
	u16 flags,
	EmergeCompletionCallback callback,
	void *callback_param)
{
	EmergeThread *thread = NULL;
	bool entry_already_exists = false;

	{
		MutexAutoLock queuelock(m_queue_mutex);

		if (!pushBlockEmergeData(blockpos, peer_id, flags,
				callback, callback_param, &entry_already_exists))
			return false;

		if (entry_already_exists)
			return true;

		thread = getOptimalThread();
		thread->pushBlock(blockpos);
	}

	thread->signal();

	return true;
}


//
// Mapgen-related helper functions
//


// TODO(hmmmm): Move this to ServerMap
v3s16 EmergeManager::getContainingChunk(v3s16 blockpos)
{
	return getContainingChunk(blockpos, mgparams->chunksize);
}

// TODO(hmmmm): Move this to ServerMap
v3s16 EmergeManager::getContainingChunk(v3s16 blockpos, s16 chunksize)
{
	s16 coff = -chunksize / 2;
	v3s16 chunk_offset(coff, coff, coff);

	return getContainerPos(blockpos - chunk_offset, chunksize)
		* chunksize + chunk_offset;
}


int EmergeManager::getSpawnLevelAtPoint(v2s16 p)
{
	if (m_mapgens.empty() || !m_mapgens[0]) {
		errorstream << "EmergeManager: getSpawnLevelAtPoint() called"
			" before mapgen init" << std::endl;
		return 0;
	}

	return m_mapgens[0]->getSpawnLevelAtPoint(p);
}


int EmergeManager::getGroundLevelAtPoint(v2s16 p)
{
	if (m_mapgens.empty() || !m_mapgens[0]) {
		errorstream << "EmergeManager: getGroundLevelAtPoint() called"
			" before mapgen init" << std::endl;
		return 0;
	}

	return m_mapgens[0]->getGroundLevelAtPoint(p);
}

// TODO(hmmmm): Move this to ServerMap
bool EmergeManager::isBlockUnderground(v3s16 blockpos)
{
	// Use a simple heuristic
	return blockpos.Y * (MAP_BLOCKSIZE + 1) <= mgparams->water_level;
}

bool EmergeManager::pushBlockEmergeData(
	v3s16 pos,
	u16 peer_requested,
	u16 flags,
	EmergeCompletionCallback callback,
	void *callback_param,
	bool *entry_already_exists)
{
	u16 &count_peer = m_peer_queue_count[peer_requested];

	if ((flags & BLOCK_EMERGE_FORCE_QUEUE) == 0) {
		if (m_blocks_enqueued.size() >= m_qlimit_total)
			return false;

		if (peer_requested != PEER_ID_INEXISTENT) {
			u16 qlimit_peer = (flags & BLOCK_EMERGE_ALLOW_GEN) ?
				m_qlimit_generate : m_qlimit_diskonly;
			if (count_peer >= qlimit_peer)
				return false;
		} else {
			// limit block enqueue requests for active blocks to 1/2 of total
			if (count_peer * 2 >= m_qlimit_total)
				return false;
		}
	}

	std::pair<std::map<v3s16, BlockEmergeData>::iterator, bool> findres;
	findres = m_blocks_enqueued.insert(std::make_pair(pos, BlockEmergeData()));

	BlockEmergeData &bedata = findres.first->second;
	*entry_already_exists   = !findres.second;

	if (callback)
		bedata.callbacks.emplace_back(callback, callback_param);

	if (*entry_already_exists) {
		bedata.flags |= flags;
	} else {
		bedata.flags = flags;
		bedata.peer_requested = peer_requested;

		count_peer++;
	}

	return true;
}


bool EmergeManager::popBlockEmergeData(v3s16 pos, BlockEmergeData *bedata)
{
	std::map<v3s16, BlockEmergeData>::iterator it;
	std::unordered_map<u16, u16>::iterator it2;

	it = m_blocks_enqueued.find(pos);
	if (it == m_blocks_enqueued.end())
		return false;

	*bedata = it->second;

	it2 = m_peer_queue_count.find(bedata->peer_requested);
	if (it2 == m_peer_queue_count.end())
		return false;

	u16 &count_peer = it2->second;
	assert(count_peer != 0);
	count_peer--;

	m_blocks_enqueued.erase(it);

	return true;
}


EmergeThread *EmergeManager::getOptimalThread()
{
	size_t nthreads = m_threads.size();

	FATAL_ERROR_IF(nthreads == 0, "No emerge threads!");

	size_t index = 0;
	size_t nitems_lowest = m_threads[0]->m_block_queue.size();

	for (size_t i = 1; i < nthreads; i++) {
		size_t nitems = m_threads[i]->m_block_queue.size();
		if (nitems < nitems_lowest) {
			index = i;
			nitems_lowest = nitems;
		}
	}

	return m_threads[index];
}


////
//// EmergeThread
////

EmergeThread::EmergeThread(Server *server, int ethreadid) :
	enable_mapgen_debug_info(false),
	id(ethreadid),
	m_server(server),
	m_map(nullptr),
	m_emerge(nullptr),
	m_mapgen(nullptr),
	m_trans_liquid(nullptr)
{
	m_name = "Emerge-" + itos(ethreadid);
}


void EmergeThread::signal()
{
	m_queue_event.signal();
}


bool EmergeThread::pushBlock(const v3s16 &pos)
{
	m_block_queue.push(pos);
	return true;
}


void EmergeThread::cancelPendingItems()
{
	MutexAutoLock queuelock(m_emerge->m_queue_mutex);

	while (!m_block_queue.empty()) {
		BlockEmergeData bedata;
		v3s16 pos;

		pos = m_block_queue.front();
		m_block_queue.pop();

		m_emerge->popBlockEmergeData(pos, &bedata);

		runCompletionCallbacks(pos, EMERGE_CANCELLED, bedata.callbacks);
	}
}


void EmergeThread::runCompletionCallbacks(const v3s16 &pos, EmergeAction action,
	const EmergeCallbackList &callbacks)
{
	for (size_t i = 0; i != callbacks.size(); i++) {
		EmergeCompletionCallback callback;
		void *param;

		callback = callbacks[i].first;
		param    = callbacks[i].second;

		callback(pos, action, param);
	}
}


bool EmergeThread::popBlockEmerge(v3s16 *pos, BlockEmergeData *bedata)
{
	MutexAutoLock queuelock(m_emerge->m_queue_mutex);

	if (m_block_queue.empty())
		return false;

	*pos = m_block_queue.front();
	m_block_queue.pop();

	m_emerge->popBlockEmergeData(*pos, bedata);

	return true;
}


EmergeAction EmergeThread::getBlockOrStartGen(
	const v3s16 &pos, bool allow_gen, MapBlock **block, BlockMakeData *bmdata)
{
	MutexAutoLock envlock(m_server->m_env_mutex);

	// 1). Attempt to fetch block from memory
	*block = m_map->getBlockNoCreateNoEx(pos);
	if (*block && !(*block)->isDummy()) {
		if ((*block)->isGenerated())
			return EMERGE_FROM_MEMORY;
	} else {
		// 2). Attempt to load block from disk if it was not in the memory
		*block = m_map->loadBlock(pos);
		if (*block && (*block)->isGenerated())
			return EMERGE_FROM_DISK;
	}

	// 3). Attempt to start generation
	if (allow_gen && m_map->initBlockMake(pos, bmdata))
		return EMERGE_GENERATED;

	// All attempts failed; cancel this block emerge
	return EMERGE_CANCELLED;
}


MapBlock *EmergeThread::finishGen(v3s16 pos, BlockMakeData *bmdata,
	std::map<v3s16, MapBlock *> *modified_blocks)
{
	MutexAutoLock envlock(m_server->m_env_mutex);
	ScopeProfiler sp(g_profiler,
		"EmergeThread: after Mapgen::makeChunk", SPT_AVG);

	/*
		Perform post-processing on blocks (invalidate lighting, queue liquid
		transforms, etc.) to finish block make
	*/
	m_map->finishBlockMake(bmdata, modified_blocks);

	MapBlock *block = m_map->getBlockNoCreateNoEx(pos);
	if (!block) {
		errorstream << "EmergeThread::finishGen: Couldn't grab block we "
			"just generated: " << PP(pos) << std::endl;
		return NULL;
	}

	v3s16 minp = bmdata->blockpos_min * MAP_BLOCKSIZE;
	v3s16 maxp = bmdata->blockpos_max * MAP_BLOCKSIZE +
				 v3s16(1,1,1) * (MAP_BLOCKSIZE - 1);

	// Ignore map edit events, they will not need to be sent
	// to anybody because the block hasn't been sent to anybody
	MapEditEventAreaIgnorer ign(
		&m_server->m_ignore_map_edit_events_area,
		VoxelArea(minp, maxp));

	/*
		Run Lua on_generated callbacks
	*/
	try {
		m_server->getScriptIface()->environment_OnGenerated(
			minp, maxp, m_mapgen->blockseed);
	} catch (LuaError &e) {
		m_server->setAsyncFatalError("Lua: finishGen" + std::string(e.what()));
	}

	/*
		Clear generate notifier events
	*/
	m_mapgen->gennotify.clearEvents();

	EMERGE_DBG_OUT("ended up with: " << analyze_block(block));

	/*
		Activate the block
	*/
	m_server->m_env->activateBlock(block, 0);

	return block;
}

bool EmergeThread::initScripting()
{
	m_script = std::make_unique<EmergeScripting>(this);

	try {
		m_script->loadMod(Server::getBuiltinLuaPath() + "/init.lua",
			BUILTIN_MOD_NAME);
		m_script->checkSetByBuiltin();
	} catch (const ModError &e) {
		errorstream << "Execution of mapgen base environment failed." << std::endl;
		m_server->setAsyncFatalError(e.what());
		return false;
	}

	const auto &list = m_server->m_mapgen_init_files;
	try {
		for (auto &it : list)
			m_script->loadMod(it.second, it.first);

		m_script->on_mods_loaded();
	} catch (const ModError &e) {
		errorstream << "Failed to load mod script inside mapgen environment." << std::endl;
		m_server->setAsyncFatalError(e.what());
		return false;
	}

	return true;
}

void *EmergeThread::run()
{
	BEGIN_DEBUG_EXCEPTION_HANDLER

	v3s16 pos;

	m_map    = (ServerMap *)&(m_server->m_env->getMap());
	m_emerge = m_server->m_emerge;
	m_mapgen = m_emerge->m_mapgens[id];
	enable_mapgen_debug_info = m_emerge->enable_mapgen_debug_info;

	if (!initScripting()) {
		m_script.reset();
		stop(); // do not enter main loop
	}

	try {
	while (!stopRequested()) {
		std::map<v3s16, MapBlock *> modified_blocks;
		BlockEmergeData bedata;
		BlockMakeData bmdata;
		EmergeAction action;
		MapBlock *block;

		if (!popBlockEmerge(&pos, &bedata)) {
			m_queue_event.wait();
			continue;
		}

		if (blockpos_over_max_limit(pos))
			continue;

		bool allow_gen = bedata.flags & BLOCK_EMERGE_ALLOW_GEN;
		EMERGE_DBG_OUT("pos=" PP(pos) " allow_gen=" << allow_gen);

		action = getBlockOrStartGen(pos, allow_gen, &block, &bmdata);
		if (action == EMERGE_GENERATED) {
			bool error = false;
			m_trans_liquid = &bmdata.transforming_liquid;
			{
				ScopeProfiler sp(g_profiler,
					"EmergeThread: Mapgen::makeChunk", SPT_AVG);

				m_mapgen->makeChunk(&bmdata);
			}

			{
				ScopeProfiler sp(g_profiler,
					"EmergeThread: Lua on_generated", SPT_AVG);

				try {
					m_script->on_generated(&bmdata);
				} catch (const LuaError &e) {
					m_server->setAsyncFatalError(e.what());
					error = true;
				}
			}

			if (!error)
				block = finishGen(pos, &bmdata, &modified_blocks);

			if (!block || error)
				action = EMERGE_ERRORED;

			m_trans_liquid = nullptr;
		}

		

		runCompletionCallbacks(pos, action, bedata.callbacks);

		if (block)
			modified_blocks[pos] = block;

		if (!modified_blocks.empty())
			m_server->SetBlocksNotSent(modified_blocks);
	}
	} catch (VersionMismatchException &e) {
		std::ostringstream err;
		err << "World data version mismatch in MapBlock " << PP(pos) << std::endl
			<< "----" << std::endl
			<< "\"" << e.what() << "\"" << std::endl
			<< "See debug.txt." << std::endl
			<< "World probably saved by a newer version of " PROJECT_NAME_C "."
			<< std::endl;
		m_server->setAsyncFatalError(err.str());
	} catch (SerializationError &e) {
		std::ostringstream err;
		err << "Invalid data in MapBlock " << PP(pos) << std::endl
			<< "----" << std::endl
			<< "\"" << e.what() << "\"" << std::endl
			<< "See debug.txt." << std::endl
			<< "You can ignore this using [ignore_world_load_errors = true]."
			<< std::endl;
		m_server->setAsyncFatalError(err.str());
	}
	
	try {
		if (m_script)
			m_script->on_shutdown();
	} catch (const ModError &e) {
		m_server->setAsyncFatalError(e.what());
	}

	END_DEBUG_EXCEPTION_HANDLER
	return NULL;
}
