/*
Minetest
Copyright (C) 2010-2013 celeron55, Perttu Ahola <celeron55@gmail.com>
Copyright (C) 2013-2020 Minetest core developers & community

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

#include "unit_sao.h"
#include "scripting_server.h"
#include "serverenvironment.h"

UnitSAO::UnitSAO(ServerEnvironment *env, v3f pos) : ServerActiveObject(env, pos)
{
	// Initialize something to armor groups
	m_armor_groups["fleshy"] = 100;
}

ServerActiveObject *UnitSAO::getParent() const
{
	if (!m_attachment_parent_id)
		return nullptr;
	// Check if the parent still exists
	ServerActiveObject *obj = m_env->getActiveObject(m_attachment_parent_id);

	return obj;
}

void UnitSAO::setArmorGroups(const ItemGroupList &armor_groups)
{
	m_armor_groups = armor_groups;
	m_armor_groups_sent = false;
}

const ItemGroupList &UnitSAO::getArmorGroups() const
{
	return m_armor_groups;
}

void UnitSAO::setAnimation(
		v2f frame_range, float frame_speed, float frame_blend, bool frame_loop)
{
	// store these so they can be updated to clients
	m_animation_range = frame_range;
	m_animation_speed = frame_speed;
	m_animation_blend = frame_blend;
	m_animation_loop = frame_loop;
	m_animation_sent = false;
}

void UnitSAO::getAnimation(v2f *frame_range, float *frame_speed, float *frame_blend,
		bool *frame_loop)
{
	*frame_range = m_animation_range;
	*frame_speed = m_animation_speed;
	*frame_blend = m_animation_blend;
	*frame_loop = m_animation_loop;
}

void UnitSAO::setAnimationSpeed(float frame_speed)
{
	m_animation_speed = frame_speed;
	m_animation_speed_sent = false;
}

void UnitSAO::setBonePosition(const std::string &bone, v3f position, v3f rotation)
{
	// store these so they can be updated to clients
	m_bone_position[bone] = core::vector2d<v3f>(position, rotation);
	m_bone_position_sent = false;
}

void UnitSAO::getBonePosition(const std::string &bone, v3f *position, v3f *rotation)
{
	*position = m_bone_position[bone].X;
	*rotation = m_bone_position[bone].Y;
}

// clang-format off
void UnitSAO::sendOutdatedData()
{
	if (!m_armor_groups_sent) {
		m_armor_groups_sent = true;
		m_messages_out.emplace(getId(), true, generateUpdateArmorGroupsCommand());
	}

	if (!m_animation_sent) {
		m_animation_sent = true;
		m_animation_speed_sent = true;
		m_messages_out.emplace(getId(), true,
			generateUpdateAnimationCommand(37),
			generateUpdateAnimationCommand(32));
	} else if (!m_animation_speed_sent) {
		// Animation speed is also sent when 'm_animation_sent == false'
		m_animation_speed_sent = true;
		m_messages_out.emplace(getId(), true,
			generateUpdateAnimationSpeedCommand(),
			// MT 0.4 has no update animation speed command
			generateUpdateAnimationCommand(32));
	}

	if (!m_bone_position_sent) {
		m_bone_position_sent = true;
		for (const auto &bone_pos : m_bone_position) {
			std::string str = generateUpdateBonePositionCommand(
				bone_pos.first, bone_pos.second.X, bone_pos.second.Y, 37);
			std::string legacy_str = generateUpdateBonePositionCommand(
				bone_pos.first, bone_pos.second.X, bone_pos.second.Y, 32);
			m_messages_out.emplace(getId(), true, str, legacy_str);
		}
	}

	if (!m_attachment_sent) {
		m_attachment_sent = true;
		m_messages_out.emplace(getId(), true,
			generateUpdateAttachmentCommand(37),
			generateUpdateAttachmentCommand(32));
	}
}
// clang-format on

void UnitSAO::setAttachment(int parent_id, const std::string &bone, v3f position,
		v3f rotation, bool force_visible)
{
	// Attachments need to be handled on both the server and client.
	// If we just attach on the server, we can only copy the position of the parent.
	// Attachments are still sent to clients at an interval so players might see them
	// lagging, plus we can't read and attach to skeletal bones. If we just attach on
	// the client, the server still sees the child at its original location. This
	// breaks some things so we also give the server the most accurate representation
	// even if players only see the client changes.

	int old_parent = m_attachment_parent_id;
	m_attachment_parent_id = parent_id;
	m_attachment_bone = bone;
	m_attachment_position = position;
	m_attachment_rotation = rotation;
	m_force_visible = force_visible;
	m_attachment_sent = false;

	if (parent_id != old_parent) {
		onDetach(old_parent);
		onAttach(parent_id);
	}
}

//used when this was occupying an reservedID, expects a 
//void UnitSAO::updateAttachmentCHILD() {
//	
//}

void UnitSAO::getAttachment(int *parent_id, std::string *bone, v3f *position,
		v3f *rotation, bool *force_visible) const
{
	*parent_id = m_attachment_parent_id;
	*bone = m_attachment_bone;
	*position = m_attachment_position;
	*rotation = m_attachment_rotation;
	*force_visible = m_force_visible;
}

void UnitSAO::clearChildAttachments()
{
	for (int child_id : m_attachment_child_ids) {
		// Child can be NULL if it was deleted earlier
		if (ServerActiveObject *child = m_env->getActiveObject(child_id))
			child->setAttachment(0, "", v3f(0, 0, 0), v3f(0, 0, 0), false);
	}
	m_attachment_child_ids.clear();
}

void UnitSAO::clearParentAttachment()
{
	ServerActiveObject *parent = nullptr;
	if (m_attachment_parent_id) {
		parent = m_env->getActiveObject(m_attachment_parent_id);
		setAttachment(0, "", m_attachment_position, m_attachment_rotation, false);
	} else {
		setAttachment(0, "", v3f(0, 0, 0), v3f(0, 0, 0), false);
	}
	// Do it
	if (parent)
		parent->removeAttachmentChild(m_id);
}

void UnitSAO::addAttachmentChild(int child_id)
{
	m_attachment_child_ids.insert(child_id);
}

void UnitSAO::removeAttachmentChild(int child_id)
{
	m_attachment_child_ids.erase(child_id);
}

const std::unordered_set<int> &UnitSAO::getAttachmentChildIds() const
{
	return m_attachment_child_ids;
}

void UnitSAO::onAttach(int parent_id)
{
	if (!parent_id)
		return;

	ServerActiveObject *parent = m_env->getActiveObject(parent_id);

	if (!parent || parent->isGone())
		return; // Do not try to notify soon gone parent

	if (parent->getType() == ACTIVEOBJECT_TYPE_LUAENTITY) {
		// Call parent's on_attach field
		m_env->getScriptIface()->luaentity_on_attach_child(parent_id, this);
	}
}

void UnitSAO::onDetach(int parent_id)
{
	if (!parent_id)
		return;

	ServerActiveObject *parent = m_env->getActiveObject(parent_id);
	if (getType() == ACTIVEOBJECT_TYPE_LUAENTITY)
		m_env->getScriptIface()->luaentity_on_detach(m_id, parent);

	if (!parent || parent->isGone())
		return; // Do not try to notify soon gone parent

	if (parent->getType() == ACTIVEOBJECT_TYPE_LUAENTITY)
		m_env->getScriptIface()->luaentity_on_detach_child(parent_id, this);
}

ObjectProperties *UnitSAO::accessObjectProperties()
{
	return &m_prop;
}

void UnitSAO::notifyObjectPropertiesModified()
{
	m_properties_sent = false;
}

std::string UnitSAO::generateUpdateAttachmentCommand(const u16 protocol_version) const
{
	std::ostringstream os(std::ios::binary);
	// command
	writeU8(os, AO_CMD_ATTACH_TO);
	// parameters
	writeS16(os, m_attachment_parent_id);
	os << serializeString16(m_attachment_bone);

	// Add/remove offsets to compensate for MT 0.4
	if (protocol_version >= 37) {
		writeV3F32(os, m_attachment_position);
	} else {
		v3f compat_attachment_position = m_attachment_position;
		if (getType() == ACTIVEOBJECT_TYPE_PLAYER) {
			compat_attachment_position.Y += BS;
		} else {
			ServerActiveObject *p =
					m_env->getActiveObject(m_attachment_parent_id);
			if (p && p->getType() == ACTIVEOBJECT_TYPE_PLAYER)
				compat_attachment_position.Y -= BS;
		}
		writeV3F1000(os, compat_attachment_position);
	}

	writeV3F(os, m_attachment_rotation, protocol_version);
	writeU8(os, m_force_visible);
	return os.str();
}

std::string UnitSAO::generateUpdateBonePositionCommand(const std::string &bone,
		const v3f &position, const v3f &rotation, const u16 protocol_version)
{
	std::ostringstream os(std::ios::binary);
	// command
	writeU8(os, AO_CMD_SET_BONE_POSITION);
	// parameters
	os << serializeString16(bone);
	writeV3F(os, position, protocol_version);
	writeV3F(os, rotation, protocol_version);
	return os.str();
}

std::string UnitSAO::generateUpdateAnimationSpeedCommand() const
{
	std::ostringstream os(std::ios::binary);
	// command
	writeU8(os, AO_CMD_SET_ANIMATION_SPEED);
	// parameters
	writeF32(os, m_animation_speed);
	return os.str();
}

std::string UnitSAO::generateUpdateAnimationCommand(const u16 protocol_version) const
{
	std::ostringstream os(std::ios::binary);
	// command
	writeU8(os, AO_CMD_SET_ANIMATION);
	// parameters
	writeV2F(os, m_animation_range, protocol_version);
	writeF(os, m_animation_speed, protocol_version);
	writeF(os, m_animation_blend, protocol_version);
	// these are sent inverted so we get true when the server sends nothing
	writeU8(os, !m_animation_loop);
	return os.str();
}

std::string UnitSAO::generateUpdateArmorGroupsCommand() const
{
	std::ostringstream os(std::ios::binary);
	writeU8(os, AO_CMD_UPDATE_ARMOR_GROUPS);
	writeU16(os, m_armor_groups.size());
	for (const auto &armor_group : m_armor_groups) {
		os << serializeString16(armor_group.first);
		writeS16(os, armor_group.second);
	}
	return os.str();
}

std::string UnitSAO::generateUpdatePositionCommand(const v3f &position,
		const v3f &velocity, const v3f &acceleration, const v3f &rotation,
		bool do_interpolate, bool is_movement_end, f32 update_interval,
		const u16 protocol_version)
{
	std::ostringstream os(std::ios::binary);
	// command
	writeU8(os, AO_CMD_UPDATE_POSITION);
	// pos
	writeV3F(os, position, protocol_version);
	// velocity
	writeV3F(os, velocity, protocol_version);
	// acceleration
	writeV3F(os, acceleration, protocol_version);
	// rotation
	if (protocol_version >= 37)
		writeV3F32(os, rotation);
	else
		writeF1000(os, rotation.Y);
	// do_interpolate
	writeU8(os, do_interpolate);
	// is_end_position (for interpolation)
	writeU8(os, is_movement_end);
	// update_interval (for interpolation)
	writeF(os, update_interval, protocol_version);
	return os.str();
}

std::string UnitSAO::generateSetPropertiesCommand(
		const ObjectProperties &prop, const u16 protocol_version) const
{
	std::ostringstream os(std::ios::binary);
	writeU8(os, AO_CMD_SET_PROPERTIES);
	prop.serialize(os, protocol_version);
	return os.str();
}

std::string UnitSAO::generatePunchCommand(u16 result_hp) const
{
	std::ostringstream os(std::ios::binary);
	// command
	writeU8(os, AO_CMD_PUNCHED);
	// result_hp
	writeU16(os, result_hp);
	return os.str();
}

std::string UnitSAO::generateLegacyPunchCommand(u16 result_hp) const
{
	std::ostringstream os(std::ios::binary);
	// command
	writeU8(os, AO_CMD_PUNCHED);
	// result_hp
	writeU16(os, result_hp);
	return os.str();
}

void UnitSAO::sendPunchCommand()
{
	const u16 result_hp = getHP();
	m_messages_out.emplace(getId(), true, generatePunchCommand(result_hp),
			generateLegacyPunchCommand(result_hp));
}
