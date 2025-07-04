/*
Minetest
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

#pragma once

#include <map>
#include <mutex>
#include "network/networkprotocol.h"
#include "irr_v3d.h"
#include "util/container.h"
#include "mapgen/mapgen.h" // for MapgenParams
#include "map.h"

#define BLOCK_EMERGE_ALLOW_GEN   (1 << 0)
#define BLOCK_EMERGE_FORCE_QUEUE (1 << 1)

#define EMERGE_DBG_OUT(x) {                            \
	if (enable_mapgen_debug_info)                      \
		infostream << "EmergeThread: " x << std::endl; \
}

class EmergeThread;
class NodeDefManager;
class Settings;

class BiomeManager;
class OreManager;
class DecorationManager;
class SchematicManager;
class Server;
class ModApiMapgen;

// Structure containing inputs/outputs for chunk generation
struct BlockMakeData {
	MMVManip *vmanip = nullptr;
	u64 seed = 0;
	v3s16 blockpos_min;
	v3s16 blockpos_max;
	UniqueQueue<v3s16> transforming_liquid;
	const NodeDefManager *nodedef = nullptr;

	BlockMakeData() = default;

	~BlockMakeData() { delete vmanip; }
};

// Result from processing an item on the emerge queue
enum EmergeAction {
	EMERGE_CANCELLED,
	EMERGE_ERRORED,
	EMERGE_FROM_MEMORY,
	EMERGE_FROM_DISK,
	EMERGE_GENERATED,
};

// Callback
typedef void (*EmergeCompletionCallback)(
	v3s16 blockpos, EmergeAction action, void *param);

typedef std::vector<
	std::pair<
		EmergeCompletionCallback,
		void *
	>
> EmergeCallbackList;

struct BlockEmergeData {
	u16 peer_requested;
	u16 flags;
	EmergeCallbackList callbacks;
};

class EmergeParams {
	friend class EmergeManager;
public:
	EmergeParams() = delete;
	~EmergeParams();
	DISABLE_CLASS_COPY(EmergeParams);

	const NodeDefManager *ndef; // shared
	bool enable_mapgen_debug_info;

	u32 gen_notify_on;
	const std::set<u32> *gen_notify_on_deco_ids; // shared
	const std::set<std::string> *gen_notify_on_custom; // shared

	BiomeGen *biomegen;
	BiomeManager *biomemgr;
	OreManager *oremgr;
	DecorationManager *decomgr;
	SchematicManager *schemmgr;
	

	inline GenerateNotifier createNotifier() const {
		return GenerateNotifier(gen_notify_on, gen_notify_on_deco_ids,
			gen_notify_on_custom);
	}


private:
	EmergeParams(EmergeManager *parent, const BiomeGen *biomegen,
		const BiomeManager *biomemgr,
		const OreManager *oremgr, const DecorationManager *decomgr,
		const SchematicManager *schemmgr);
};

class EmergeManager {
	/* The mod API needs unchecked access to allow:
	 * - using decomgr or oremgr to place decos/ores
	 * - using schemmgr to load and place schematics
	 */
	friend class ModApiMapgen;
public:
	const NodeDefManager *ndef;
	bool enable_mapgen_debug_info;

	// Generation Notify
	u32 gen_notify_on = 0;
	std::set<u32> gen_notify_on_deco_ids;
	std::set<std::string> gen_notify_on_custom;

	// Parameters passed to mapgens owned by ServerMap
	// TODO(hmmmm): Remove this after mapgen helper methods using them
	// are moved to ServerMap
	MapgenParams *mgparams;

	// Hackish workaround:
	// For now, EmergeManager must hold onto a ptr to the Map's setting manager
	// since the Map can only be accessed through the Environment, and the
	// Environment is not created until after script initialization.
	MapSettingsManager *map_settings_mgr;

	// Methods
	EmergeManager(Server *server);
	~EmergeManager();
	DISABLE_CLASS_COPY(EmergeManager);

	const BiomeGen *getBiomeGen() const { return biomegen; }

	// no usage restrictions
	const BiomeManager *getBiomeManager() const { return biomemgr; }
	const OreManager *getOreManager() const { return oremgr; }
	const DecorationManager *getDecorationManager() const { return decomgr; }
	const SchematicManager *getSchematicManager() const { return schemmgr; }
	// only usable before mapgen init
	
	BiomeManager *getWritableBiomeManager();
	OreManager *getWritableOreManager();
	DecorationManager *getWritableDecorationManager();
	SchematicManager *getWritableSchematicManager();

	void initMapgens(MapgenParams *mgparams);

	void startThreads();
	void stopThreads();
	bool isRunning();

	bool enqueueBlockEmerge(
		session_t peer_id,
		v3s16 blockpos,
		bool allow_generate,
		bool ignore_queue_limits=false);

	bool enqueueBlockEmergeEx(
		v3s16 blockpos,
		session_t peer_id,
		u16 flags,
		EmergeCompletionCallback callback,
		void *callback_param);

	v3s16 getContainingChunk(v3s16 blockpos);

	Mapgen *getCurrentMapgen();

	// Mapgen helpers methods
	int getSpawnLevelAtPoint(v2s16 p);
	int getGroundLevelAtPoint(v2s16 p);
	bool isBlockUnderground(v3s16 blockpos);

	static v3s16 getContainingChunk(v3s16 blockpos, s16 chunksize);

private:
	std::vector<Mapgen *> m_mapgens;
	std::vector<EmergeThread *> m_threads;
	bool m_threads_active = false;

	std::mutex m_queue_mutex;
	std::map<v3s16, BlockEmergeData> m_blocks_enqueued;
	std::unordered_map<u16, u16> m_peer_queue_count;

	u16 m_qlimit_total;
	u16 m_qlimit_diskonly;
	u16 m_qlimit_generate;

	// Managers of various map generation-related components
	// Note that each Mapgen gets a copy(!) of these to work with
	BiomeGen *biomegen;
	BiomeManager *biomemgr;
	OreManager *oremgr;
	DecorationManager *decomgr;
	SchematicManager *schemmgr;

	// Requires m_queue_mutex held
	EmergeThread *getOptimalThread();

	bool pushBlockEmergeData(
		v3s16 pos,
		u16 peer_requested,
		u16 flags,
		EmergeCompletionCallback callback,
		void *callback_param,
		bool *entry_already_exists);

	bool popBlockEmergeData(v3s16 pos, BlockEmergeData *bedata);

	friend class EmergeThread;
};
