/*
Minetest
Copyright (C) 2014-2020 paramat
Copyright (C) 2014-2016 kwolekr, Ryan Kwolek <kwolekr@minetest.net>

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

#include "objdef.h"
#include "nodedef.h"
#include "noise.h"

class Server;
class Settings;
class BiomeManager;

////
//// Biome
////

typedef u16 biome_t;

#define BIOME_NONE ((biome_t)0)

enum BiomeType {
	BIOMETYPE_NORMAL,
};

class Biome : public ObjDef, public NodeResolver {
public:
	ObjDef *clone() const;

	u32 flags;

	content_t c_top;
	content_t c_filler;
	content_t c_stone;
	content_t c_water_top;
	content_t c_water;
	content_t c_river_water;
	content_t c_riverbed;
	content_t c_dust;
	std::vector<content_t> c_cave_liquid;
	content_t c_dungeon;
	content_t c_dungeon_alt;
	content_t c_dungeon_stair;

	s16 depth_top;
	s16 depth_filler;
	s16 depth_water_top;
	s16 depth_riverbed;

	v3s16 min_pos;
	v3s16 max_pos;
	float heat_point;
	float humidity_point;
	s16 vertical_blend;

	virtual void resolveNodeNames();
};


////
//// BiomeGen
////

enum BiomeGenType {
	BIOMEGEN_ORIGINAL,
};

struct BiomeParams {
	virtual void readParams(const Settings *settings) = 0;
	virtual void writeParams(Settings *settings) const = 0;
	virtual ~BiomeParams() = default;

	s32 seed;
};

// WARNING: this class is not thread-safe
class BiomeGen {
public:
	virtual ~BiomeGen() = default;

	virtual BiomeGenType getType() const = 0;

	virtual BiomeGen *clone(BiomeManager *biomemgr) const = 0;

	// Calculates the biome at the exact position provided.  This function can
	// be called at any time, but may be less efficient than the latter methods,
	// depending on implementation.
	virtual Biome *calcBiomeAtPoint(v3s16 pos) const = 0;

	// Computes any intermediate results needed for biome generation.  Must be
	// called before using any of: getBiomes, getBiomeAtPoint, or getBiomeAtIndex.
	// Calling this invalidates the previous results stored in biomemap.
	virtual void calcBiomeNoise(v3s16 pmin) = 0;

	// Gets all biomes in current chunk using each corresponding element of
	// heightmap as the y position, then stores the results by biome index in
	// biomemap (also returned)
	virtual biome_t *getBiomes(s16 *heightmap, v3s16 pmin) = 0;

	// Gets a single biome at the specified position, which must be contained
	// in the region formed by m_pmin and (m_pmin + m_csize - 1).
	virtual Biome *getBiomeAtPoint(v3s16 pos) const = 0;

	// Same as above, but uses a raw numeric index correlating to the (x,z) position.
	virtual Biome *getBiomeAtIndex(size_t index, v3s16 pos) const = 0;

	// Result of calcBiomes bulk computation.
	biome_t *biomemap = nullptr;

protected:
	BiomeManager *m_bmgr = nullptr;
	v3s16 m_pmin;
	v3s16 m_csize;
};


////
//// BiomeGen implementations
////

//
// Original biome algorithm (Whittaker's classification + surface height)
//

struct BiomeParamsOriginal : public BiomeParams {
	BiomeParamsOriginal() :
		np_heat(50, 50, v3f(1000.0, 1000.0, 1000.0), 5349, 3, 0.5, 2.0),
		np_humidity(50, 50, v3f(1000.0, 1000.0, 1000.0), 842, 3, 0.5, 2.0),
		np_heat_blend(0, 1.5, v3f(8.0, 8.0, 8.0), 13, 2, 1.0, 2.0),
		np_humidity_blend(0, 1.5, v3f(8.0, 8.0, 8.0), 90003, 2, 1.0, 2.0)
	{
	}

	virtual void readParams(const Settings *settings);
	virtual void writeParams(Settings *settings) const;

	NoiseParams np_heat;
	NoiseParams np_humidity;
	NoiseParams np_heat_blend;
	NoiseParams np_humidity_blend;
};

class BiomeGenOriginal : public BiomeGen {
public:
	BiomeGenOriginal(BiomeManager *biomemgr,
		BiomeParamsOriginal *params, v3s16 chunksize);
	virtual ~BiomeGenOriginal();

	BiomeGenType getType() const { return BIOMEGEN_ORIGINAL; }

	float calcHeatAtPoint(v3s16 pos) const;
	float calcHumidityAtPoint(v3s16 pos) const;

	BiomeGen *clone(BiomeManager *biomemgr) const;

	Biome *calcBiomeAtPoint(v3s16 pos) const;
	void calcBiomeNoise(v3s16 pmin);

	biome_t *getBiomes(s16 *heightmap, v3s16 pmin);
	Biome *getBiomeAtPoint(v3s16 pos) const;
	Biome *getBiomeAtIndex(size_t index, v3s16 pos) const;

	Biome *calcBiomeFromNoise(float heat, float humidity, v3s16 pos) const;

	float *heatmap;
	float *humidmap;

private:
	BiomeParamsOriginal *m_params;

	Noise *noise_heat;
	Noise *noise_humidity;
	Noise *noise_heat_blend;
	Noise *noise_humidity_blend;
};


////
//// BiomeManager
////

class BiomeManager : public ObjDefManager {
public:
	BiomeManager(Server *server);
	virtual ~BiomeManager() = default;

	BiomeManager *clone() const;

	const char *getObjectTitle() const
	{
		return "biome";
	}

	static Biome *create(BiomeType type)
	{
		return new Biome;
	}

	BiomeGen *createBiomeGen(BiomeGenType type, BiomeParams *params, v3s16 chunksize)
	{
		switch (type) {
		case BIOMEGEN_ORIGINAL:
			return new BiomeGenOriginal(this,
				(BiomeParamsOriginal *)params, chunksize);
		default:
			return NULL;
		}
	}

	static BiomeParams *createBiomeParams(BiomeGenType type)
	{
		switch (type) {
		case BIOMEGEN_ORIGINAL:
			return new BiomeParamsOriginal;
		default:
			return NULL;
		}
	}

	virtual void clear();

	// For BiomeGen type 'BiomeGenOriginal'
	float getHeatAtPosOriginal(v3s16 pos, NoiseParams &np_heat,
		NoiseParams &np_heat_blend, u64 seed) const;
	float getHumidityAtPosOriginal(v3s16 pos, NoiseParams &np_humidity,
		NoiseParams &np_humidity_blend, u64 seed) const;
	const Biome *getBiomeFromNoiseOriginal(float heat, float humidity,
		v3s16 pos) const;

private:
	BiomeManager() {};

	Server *m_server;

};
