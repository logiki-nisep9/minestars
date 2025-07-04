/*
MineStars - MultiCraft - Minetest/Luanti
Copyright (C) 2025 Logiki, <donatto555@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation; either version 2.1 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

//The purpose of this file is to replace the clientiface functions when this server acts like a slave

#include "slave_helpers.h"

#include <sstream>
#include "network/mt_connection.h"
#include "network/serveropcodes.h"
#include "remoteplayer.h"
#include "settings.h"
#include "mapblock.h"
#include "serverenvironment.h"
#include "map.h"
#include "emerge.h"
#include "server/luaentity_sao.h"
#include "server/player_sao.h"
#include "log.h"
#include "util/srp.h"
#include "face_position_cache.h"

void ClientDataHelper::SentBlock(v3s16 p)
{
	if (m_blocks_sending.find(p) == m_blocks_sending.end())
		m_blocks_sending[p] = 0.0f;
	else
		infostream << "ClientDataHelper::SentBlock(): Sent block already in m_blocks_sending" << std::endl;
}

LuaEntitySAO *getAttachedObject__(PlayerSAO *sao, ServerEnvironment *env)
{
	if (!sao->isAttached())
		return nullptr;

	int id;
	std::string bone;
	v3f dummy;
	bool force_visible;
	sao->getAttachment(&id, &bone, &dummy, &dummy, &force_visible);
	ServerActiveObject *ao = env->getActiveObject(id);
	while (id && ao) {
		ao->getAttachment(&id, &bone, &dummy, &dummy, &force_visible);
		if (id)
			ao = env->getActiveObject(id);
	}
	return dynamic_cast<LuaEntitySAO *>(ao);
}

void ClientDataHelper::GetNextBlocks(ServerEnvironment *env, EmergeManager * emerge, float dtime, std::vector<PrioritySortedBlockTransferPID> &dest) {
	// Increment timers
	m_nothing_to_send_pause_timer -= dtime;

	if (m_nothing_to_send_pause_timer >= 0)
		return;

	RemotePlayer *player = env->getPlayer(playerid);
	// This can happen sometimes; clients and players are not in perfect sync.
	if (!player)
		return;

	PlayerSAO *sao = player->getPlayerSAO();
	if (!sao)
		return;

	// Won't send anything if already sending
	if (m_blocks_sending.size() >= m_max_simul_sends) {
		//infostream << "Not sending any blocks, Queue full." <<std::endl;
		return;
	}

	v3f playerpos = sao->getBasePosition();
	// if the player is attached, get the velocity from the attached object
	LuaEntitySAO *lsao = getAttachedObject__(sao, env);
	const v3f &playerspeed = lsao? lsao->getVelocity() : player->getSpeed();
	v3f playerspeeddir(0,0,0);
	if (playerspeed.getLength() > 1.0f * BS)
		playerspeeddir = playerspeed / playerspeed.getLength();
	// Predict to next block
	v3f playerpos_predicted = playerpos + playerspeeddir * (MAP_BLOCKSIZE * BS);

	v3s16 center_nodepos = floatToInt(playerpos_predicted, BS);

	v3s16 center = getNodeBlockPos(center_nodepos);

	// Camera position and direction
	v3f camera_pos = sao->getEyePosition();
	v3f camera_dir = v3f(0,0,1);
	camera_dir.rotateYZBy(sao->getLookPitch());
	camera_dir.rotateXZBy(sao->getRotation().Y);

	u16 max_simul_sends_usually = m_max_simul_sends;

	/*
		Check the time from last addNode/removeNode.

		Decrease send rate if player is building stuff.
	*/
	m_time_from_building += dtime;
	if (m_time_from_building < m_min_time_from_building) {
		max_simul_sends_usually = LIMITED_MAX_SIMULTANEOUS_BLOCK_SENDS;
	}

	/*
		Number of blocks sending + number of blocks selected for sending
	*/
	u32 num_blocks_selected = m_blocks_sending.size();

	/*
		next time d will be continued from the d from which the nearest
		unsent block was found this time.

		This is because not necessarily any of the blocks found this
		time are actually sent.
	*/
	s32 new_nearest_unsent_d = -1;

	// Get view range and camera fov (radians) from the client
	s16 wanted_range = sao->getWantedRange() + 1;
	float camera_fov = sao->getFov();

	/*
		Get the starting value of the block finder radius.
	*/
	if (m_last_center != center) {
		m_nearest_unsent_d = 0;
		m_last_center = center;
	}
	// reset the unsent distance if the view angle has changed more that 10% of the fov
	// (this matches isBlockInSight which allows for an extra 10%)
	if (camera_dir.dotProduct(m_last_camera_dir) < std::cos(camera_fov * 0.1f)) {
		m_nearest_unsent_d = 0;
		m_last_camera_dir = camera_dir;
	}
	if (m_nearest_unsent_d > 0) {
		// make sure any blocks modified since the last time we sent blocks are resent
		for (const v3s16 &p : m_blocks_modified) {
			m_nearest_unsent_d = std::min(m_nearest_unsent_d, center.getDistanceFrom(p));
		}
	}
	m_blocks_modified.clear();

	s16 d_start = m_nearest_unsent_d;

	// Distrust client-sent FOV and get server-set player object property
	// zoom FOV (degrees) as a check to avoid hacked clients using FOV to load
	// distant world.
	// (zoom is disabled by value 0)
	float prop_zoom_fov = sao->getZoomFOV() < 0.001f ?
		0.0f :
		std::max(camera_fov, sao->getZoomFOV() * core::DEGTORAD);

	const s16 full_d_max = std::min(adjustDist(m_max_send_distance, prop_zoom_fov),
		wanted_range);
	const s16 d_opt = std::min(adjustDist(m_block_optimize_distance, prop_zoom_fov),
		wanted_range);
	const s16 d_blocks_in_sight = full_d_max * BS * MAP_BLOCKSIZE;

	s16 d_max_gen = std::min(adjustDist(m_max_gen_distance, prop_zoom_fov),
		wanted_range);

	s16 d_max = full_d_max;

	// Don't loop very much at a time
	s16 max_d_increment_at_time = 2;
	if (d_max > d_start + max_d_increment_at_time)
		d_max = d_start + max_d_increment_at_time;

	// cos(angle between velocity and camera) * |velocity|
	// Limit to 0.0f in case player moves backwards.
	f32 dot = rangelim(camera_dir.dotProduct(playerspeed), 0.0f, 300.0f);

	// Reduce the field of view when a player moves and looks forward.
	// limit max fov effect to 50%, 60% at 20n/s fly speed
	camera_fov = camera_fov / (1 + dot / 300.0f);

	s32 nearest_emerged_d = -1;
	s32 nearest_emergefull_d = -1;
	s32 nearest_sent_d = -1;
	//bool queue_is_full = false;

	const v3s16 cam_pos_nodes = floatToInt(camera_pos, BS);

	s16 d;
	for (d = d_start; d <= d_max; d++) {
		/*
			Get the border/face dot coordinates of a "d-radiused"
			box
		*/
		std::vector<v3s16> list = FacePositionCache::getFacePositions(d);

		std::vector<v3s16>::iterator li;
		for (li = list.begin(); li != list.end(); ++li) {
			v3s16 p = *li + center;

			/*
				Send throttling
				- Don't allow too many simultaneous transfers
				- EXCEPT when the blocks are very close

				Also, don't send blocks that are already flying.
			*/

			// Start with the usual maximum
			u16 max_simul_dynamic = max_simul_sends_usually;

			// If block is very close, allow full maximum
			if (d <= BLOCK_SEND_DISABLE_LIMITS_MAX_D)
				max_simul_dynamic = m_max_simul_sends;

			// Don't select too many blocks for sending
			if (num_blocks_selected >= max_simul_dynamic) {
				//queue_is_full = true;
				goto queue_full_break;
			}

			// Don't send blocks that are currently being transferred
			if (m_blocks_sending.find(p) != m_blocks_sending.end())
				continue;

			/*
				Do not go over max mapgen limit
			*/
			if (blockpos_over_max_limit(p))
				continue;

			// If this is true, inexistent block will be made from scratch
			bool generate = d <= d_max_gen;

			/*
				Don't generate or send if not in sight
				FIXME This only works if the client uses a small enough
				FOV setting. The default of 72 degrees is fine.
				Also retrieve a smaller view cone in the direction of the player's
				movement.
				(0.1 is about 4 degrees)
			*/
			f32 dist;
			if (!(isBlockInSight(p, camera_pos, camera_dir, camera_fov,
						d_blocks_in_sight, &dist) ||
					(playerspeed.getLength() > 1.0f * BS &&
					isBlockInSight(p, camera_pos, playerspeeddir, 0.1f,
						d_blocks_in_sight)))) {
				continue;
			}

			/*
				Don't send already sent blocks
			*/
			if (m_blocks_sent.find(p) != m_blocks_sent.end())
				continue;

			/*
				Check if map has this block
			*/
			MapBlock *block = env->getMap().getBlockNoCreateNoEx(p);

			bool block_not_found = false;
			if (block) {
				// Reset usage timer, this block will be of use in the future.
				block->resetUsageTimer();

				// Check whether the block exists (with data)
				if (block->isDummy() || !block->isGenerated())
					block_not_found = true;

				/*
					If block is not close, don't send it unless it is near
					ground level.

					Block is near ground level if night-time mesh
					differs from day-time mesh.
				*/
				if (d >= d_opt) {
					if (!block->getIsUnderground() && !block->getDayNightDiff())
						continue;
				}

				if (m_occ_cull && !block_not_found &&
						env->getMap().isBlockOccluded(block, cam_pos_nodes)) {
					continue;
				}
			}

			/*
				If block has been marked to not exist on disk (dummy) or is
				not generated and generating new ones is not wanted, skip block.
			*/
			if (!generate && block_not_found) {
				// get next one.
				continue;
			}

			/*
				Add inexistent block to emerge queue.
			*/
			if (block == NULL || block_not_found) {
				if (emerge->enqueueBlockEmerge(PEER_ID_INEXISTENT, p, generate)) {
					if (nearest_emerged_d == -1)
						nearest_emerged_d = d;
				} else {
					if (nearest_emergefull_d == -1)
						nearest_emergefull_d = d;
					goto queue_full_break;
				}

				// get next one.
				continue;
			}

			if (nearest_sent_d == -1)
				nearest_sent_d = d;

			/*
				Add block to send queue
			*/
			PrioritySortedBlockTransferPID q((float)dist, p, playerid);

			dest.push_back(q);

			num_blocks_selected += 1;
		}
	}
queue_full_break:

	// If nothing was found for sending and nothing was queued for
	// emerging, continue next time browsing from here
	if (nearest_emerged_d != -1) {
		new_nearest_unsent_d = nearest_emerged_d;
	} else if (nearest_emergefull_d != -1) {
		new_nearest_unsent_d = nearest_emergefull_d;
	} else {
		if (d > full_d_max) {
			new_nearest_unsent_d = 0;
			m_nothing_to_send_pause_timer = 2.0f;
		} else {
			if (nearest_sent_d != -1)
				new_nearest_unsent_d = nearest_sent_d;
			else
				new_nearest_unsent_d = d;
		}
	}

	if (new_nearest_unsent_d != -1)
		m_nearest_unsent_d = new_nearest_unsent_d;
}

void ClientDataHelper::SetBlockNotSent(v3s16 p)
{
	m_nothing_to_send_pause_timer = 0;

	// remove the block from sending and sent sets,
	// and mark as modified if found
	if (m_blocks_sending.erase(p) + m_blocks_sent.erase(p) > 0)
		m_blocks_modified.insert(p);
}

void ClientDataHelper::SetBlocksNotSent(std::map<v3s16, MapBlock*> &blocks)
{
	m_nothing_to_send_pause_timer = 0;

	for (auto &block : blocks) {
		v3s16 p = block.first;
		// remove the block from sending and sent sets,
		// and mark as modified if found
		if (m_blocks_sending.erase(p) + m_blocks_sent.erase(p) > 0)
			m_blocks_modified.insert(p);
	}
}

u64 ClientDataHelper::uptime() const
{
	return porting::getTimeS() - m_connection_time;
}

void ClientDataHelper::step(float dtime)
{
}

void ClientDataHelper::GotBlock(v3s16 p)
{
	if (m_blocks_sending.find(p) != m_blocks_sending.end()) {
		m_blocks_sending.erase(p);
		// only add to sent blocks if it actually was sending
		// (it might have been modified since)
		m_blocks_sent.insert(p);
	} else {
		m_excess_gotblocks++;
	}
}

void ClientDataHelper::ResendBlockIfOnWire(v3s16 p)
{
	// if this block is on wire, mark it for sending again as soon as possible
	if (m_blocks_sending.find(p) != m_blocks_sending.end()) {
		SetBlockNotSent(p);
	}
}