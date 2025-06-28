/*
 M * i*neStars - MultiCraft - Minetest/Luanti
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

#include <sys/socket.h>
#include <unistd.h>
#include <sys/un.h>

#include "NetworkEngine.h"
#include "../debug.h"
#include "../log.h"

void *NetworkCompressor::run()
{
    BEGIN_DEBUG_EXCEPTION_HANDLER
    while (!stopRequested()) {
        mutex.lock();
        std::deque<NetworkPacket> queue = std::move(queue_);
        mutex.unlock();
        if (queue.size() > 0) {
            NetworkPacket pkt; //Special numer
            verbosestream << "Compressing " << queue.size() << " packets to send into main server" << std::endl;
            std::stringstream ss;
            //u8 raw_data[];
            u16 packets = 0;
            std::vector<u8> data;
            u32 sizeF = 5;

            data.resize(5);
            writeU16(&data[0], 0x9a);
            writeU8(&data[2], 0); //Marks if the packet was divided or not
            writeU16(&data[3], (u16)queue.size());

            //[0]X [1]X [2]Y [3]Y

            //We will compress the packets on the critical zone, so we can see the raw data and concatenate all to form a single packet
            while (!queue.empty()) {
                packets++;
                NetworkPacket pkt = queue.front();
                queue.pop_front();
                u8 *raw_data = pkt.getU8Ptr(0); //Get with his command so we can see how big is it
                u32 size = pkt.getSize();
                ss << "\nPacket '";
                ss << packets;
                ss << "' size: ";
                ss << size+4;
                ss << ", CMD: ";
                ss << pkt.getCommand();
                ss << ";";
                data.resize(sizeF+pkt.getSize()+4+2); //Former size + raw packet size + command size + sizeof(uint16_t)::[SIZE of PACKET]

                writeU32(&data[sizeF], size+2); //SIZE: raw packet size + commmand size

                writeU16(&data[sizeF+4], (u16)pkt.getCommand()); //CMD: command
                memcpy(&data[sizeF+6], &raw_data[0], size);  //RAW DATA

                sizeF += size + 6;
            }
            verbosestream << FUNCTION_NAME << " -> Summary: " << ss.str() << std::endl;

            //Raw write all 65535

            //u8 *p = (u8*)&data[0];
            pkt.putRawPacket((u8*)data.data(), data.size(), (session_t)1);

            //
            std::vector<NetworkPacket> to_send;
            if (pkt.getSize() > 5000) {
                //If the packet are too big to be sent (limit=16777216bytes) we will divide the packets, with a size of 5000bytes-max
                //It really will be 5004 bytes as the size byte we need and the order for mini packets
                warningstream << "Packet reaches limit of size: " << pkt.getSize() << ", limiting" << std::endl;
                if (pkt.getSize() > 700000) {
                    errorstream << "Packet too big" << std::endl;
                }
                //u32 remaining_size = pkt.getSize();
                u32 total_size = pkt.getSize();
                u16 sb_ = (total_size/5000)+1; //+2
                //u8 fuckall = 2;
                u32 read_size = 0;
                u16 order_num = 0;
                std::vector<u16> counts;
                for (u8 i = 0; i < sb_; i++) {
                    order_num++;
                    //Get the 5000bytes of the packet to form a single packet and push into to_send;
                    u8 *raw_data_ = nullptr;
                    try {
                        verbosestream << FUNCTION_NAME << ": Program: read_size=" << read_size << std::endl;
                        raw_data_ = pkt.getU8Ptr(read_size);
                    } catch (std::exception &e) {
                        //This can occur every time.
                        //We need the last bytes to be an complete packet
                        verbosestream << FUNCTION_NAME << ": catch(exception)" << std::endl;
                        //fuckall = 0;
                        break;
                    }
                    std::vector<u8> data_RW;
                    data_RW.resize(2+2+2+1+5000);
                    //First number(u16): Order, Second number(u16): Size, Raw data: Raw data.

                    writeU16(&data_RW[0], 0x9a);          // 2 X
                    writeU8(&data_RW[2], 1);              // 1 _
                    writeU16(&data_RW[3], sb_);           // 2 _
                    writeU16(&data_RW[5], order_num);     // 2 _
                    memcpy(&data_RW[7], &raw_data_[0], 5000); //-
                    //writeU16(&data_RW[6], data_RW.size()-(2+1+1+2)); //2
                    read_size += data_RW.size()-7; //its the total size
                    NetworkPacket tmp;
                    tmp.putRawPacket((u8*)data_RW.data(), data_RW.size(), (session_t)1);
                    verbosestream << FUNCTION_NAME << ": ToSend: Packed: " << order_num << ", <size=" << data_RW.size() << ", t_size=" << tmp.getSize() << ">" << std::endl;
                    to_send.push_back(tmp);
                }
                for (NetworkPacket &pkt_i : to_send) {
                    m_server->Send(&pkt_i);
                }
            } else {
                m_server->Send(&pkt);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    END_DEBUG_EXCEPTION_HANDLER
    return nullptr;
}

void NetworkCompressor::queuePacket(NetworkPacket pkt) {
    mutex.lock();
    queue_.push_back(pkt);
    mutex.unlock();
}

const uint32_t MAX_PACKET_SIZE = 64*1024*64;

void *NetworkThread::run() {
    BEGIN_DEBUG_EXCEPTION_HANDLER
    if (!m_server->SocketConn) {
        while (!stopRequested()) {
            try {
                m_server->Receive(); //When receiving packets of movements, must be sent directly to other players without any control
            } catch (con::PeerNotFoundException &e) {
                infostream<<"Server: PeerNotFoundException"<<std::endl;
            } catch (ClientNotFoundException &e) {
            } catch (con::ConnectionBindFailed &e) {
                m_server->setAsyncFatalError(e.what());
            }
        }
    } else {
        //Connect socket N continue
        //Make connection
        //errorstream <<m_server->SocketID<<std::endl;
        actionstream << "Going to connect to socket!" << std::endl;
        if (listen(m_server->SocketID, 1) == -1) {
            errorstream << "Unable to listen to main socket for server: " << std::endl;
            return nullptr;
        }
        actionstream << "Listening complete!" << std::endl;
        int client = accept(m_server->SocketID, nullptr, nullptr);
        if (client == -1) {
            errorstream << "Unable to listen to client in socket for server" << std::endl;
            return nullptr;
        }
        actionstream << "Connected!" << std::endl;
        sockaddr_un client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        NetworkPacket pkt;
        m_parent_skt.store(client);
        while (!stopRequested()) {
            warningstream << "Waiting" << std::endl;
            std::vector<uint8_t> buffer(MAX_PACKET_SIZE);
            ssize_t bytes_received = recv(client, &buffer[0], MAX_PACKET_SIZE, 0);
            if (bytes_received > 0) {
                warningstream << FUNCTION_NAME << ": Received: " << bytes_received << std::endl;
                pkt.putRawPacket(buffer.data(), bytes_received, 0);
                _mutex.lock();
                packets.push_back(pkt);
                _mutex.unlock();
            }
        }
    }
    END_DEBUG_EXCEPTION_HANDLER
    return nullptr;
}

void *NetworkThread::getPacket() {
    _mutex.lock();
    NetworkPacket pkt;
    if (!packets.empty()) {
        pkt = (packets.size() > 0) ? packets.front() : NetworkPacket();
        packets.pop_front();
    }
    _mutex.unlock();
    return pkt;
}
