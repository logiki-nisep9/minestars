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

#pragma once

#define ARRLEN(x) (sizeof(x) / sizeof((x)[0]))

#define MYMIN(a, b) ((a) < (b) ? (a) : (b))

#define MYMAX(a, b) ((a) > (b) ? (a) : (b))

// Requires <algorithm>
#define CONTAINS(c, v) (std::find((c).begin(), (c).end(), (v)) != (c).end())

// To disable copy constructors and assignment operations for some class
// 'Foobar', add the macro DISABLE_CLASS_COPY(Foobar) as a private member.
// Note this also disables copying for any classes derived from 'Foobar' as well
// as classes having a 'Foobar' member.
#define DISABLE_CLASS_COPY(C)        \
	C(const C &) = delete;           \
	C &operator=(const C &) = delete;

// If you have used DISABLE_CLASS_COPY with a class but still want to permit moving
// use this macro to add the default move constructors back.
#define ALLOW_CLASS_MOVE(C)      \
	C(C &&other) = default;      \
	C &operator=(C &&) = default;

#ifndef _MSC_VER
	#define UNUSED_ATTRIBUTE __attribute__ ((unused))
#else
	#define UNUSED_ATTRIBUTE
#endif

// Fail compilation if condition expr is not met.
// Note that 'msg' must follow the format of a valid identifier, e.g.
// STATIC_ASSERT(sizeof(foobar_t) == 40), foobar_t_is_wrong_size);
#define STATIC_ASSERT(expr, msg) \
	UNUSED_ATTRIBUTE typedef char msg[!!(expr) * 2 - 1]

// Macros to facilitate writing position vectors to a stream
// Usage:
//	v3s16 pos(1,2,3);
//	mystream << "message " << PP(pos) << std::endl;

#define PP(x) "("<<(x).X<<","<<(x).Y<<","<<(x).Z<<")"

#define PP2(x) "("<<(x).X<<","<<(x).Y<<")"
