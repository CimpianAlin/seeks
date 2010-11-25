/**
 * This is the p2p messaging component of the Seeks project,
 * a collaborative websearch overlay network.
 *
 * Copyright (C) 2006, 2010  Emmanuel Benazera, juban@free.fr
 *
  * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _PCREPOSIX_H // avoid pcreposix.h conflict with regex.h used by gtest
#include <gtest/gtest.h>

#include "DHTNode.h"
#include "errlog.h"

using namespace dht;
using sp::errlog;

const std::string net_addr = "localhost";
const short net_port = 11000; //TODO: 0 for first available port.

const std::string pred_key = "33c80b886cdd2eb535b649817a2c24e6fe63820d";
const std::string pred_addr = "localhost";
const short pred_port = 12000;

const std::string succ_key = "4535fe26b86398a7cddf6047e9c9404074f20a8d";
const std::string succ_addr = "localhost";
const short succ_port = 13000;

/* 9ae34be97ae732e5319ce2992cb489eea99f18ed
12b10bf4851cb0b569eee7023b79db6aecfc7bed
d17b8d98350865e759426ade8899e77cc970f11d
c8eb560e300210de9be0ecc84c7789c24043bc9d
275d59a3083ac6fa87b40130de276620a621879d
8b3715ee2b5aa49854dbd4c09a5883b93afa135d
faad1ea5e0e5c5ab54bf5225279ce509f3cb5edd
7da78291e4d8fe6a8fea412fe45701a0b137ac3d
6e67359c3d66fbb024ba788e50ebd5770ad45b3d */

class ProtocolTest : public testing::Test
{
 protected:
   ProtocolTest()
     :_net_addr("localhost"),_net_port(11000),_dnode(NULL),
      _na_dnode(NULL),_v1node(NULL)
     {
     }
   
   virtual ~ProtocolTest()
     {
     }
      
   virtual void SetUp()
     {
	// init logging module.
	errlog::init_log_module();
	errlog::set_debug_level(LOG_LEVEL_ERROR | LOG_LEVEL_DHT);
	
	DHTNode::_dht_config = new dht_configuration(DHTNode::_dht_config_filename);
	DHTNode::_dht_config->_nvnodes = 1; // a single virtual node.
	
	_na_dnode = new NetAddress(_net_addr,_net_port);
	_dnode = new DHTNode(_na_dnode->getNetAddress().c_str(),_na_dnode->getPort(),false); //TODO: set node key.
	_dnode->_stabilizer = new Stabilizer(false); // empty stabilizer, not started.
	_dnode->create_vnodes();
	
	ASSERT_EQ(1,_dnode->_vnodes.size());
	_v1node = (*_dnode->_vnodes.begin()).second;
	std::cout << "vnode idkey: " << _v1node->getIdKey() << std::endl;
     
	_dnode->init_sorted_vnodes();
	_dnode->init_server();
	_dnode->_l1_server->run_thread();
     }
   
   virtual void TearDown()
     {
	delete _dnode; // stop server, client, stabilizer, hibernates & destroys vnodes.
     }
   
   std::string _net_addr;
   unsigned short _net_port;
   DHTNode *_dnode;
   NetAddress *_na_dnode;
   DHTVirtualNode *_v1node;
};

TEST_F(ProtocolTest, chord_protocol)
{
   // test no successor found.
   //TODO: while.
   DHTKey dkres;
   NetAddress nares;
   dht_err status = DHT_ERR_COM_TIMEOUT;
   while(status == DHT_ERR_COM_TIMEOUT)
     _dnode->_l1_client->RPC_getSuccessor(_v1node->getIdKey(),*_na_dnode,
						dkres,nares,
						status);
   ASSERT_EQ(DHT_ERR_NO_SUCCESSOR_FOUND,status);
   ASSERT_EQ(DHT_ERR_NO_SUCCESSOR_FOUND,status);
   
   /*- test get predecessor RPC. -*/
   // set predecessor
   DHTKey pred_dhtkey = DHTKey::from_rstring(pred_key);
   NetAddress *na_pred = new NetAddress(pred_addr,pred_port);
   _v1node->setPredecessor(pred_dhtkey,*na_pred);
   
   // test get_predecessor
   status = 0;
   _dnode->_l1_client->RPC_getPredecessor(_v1node->getIdKey(),*_na_dnode,
						dkres,nares,
						status);
   ASSERT_EQ(DHT_ERR_OK,status);
   ASSERT_EQ(pred_key,dkres.to_rstring());
   ASSERT_EQ(nares.getNetAddress(),na_pred->getNetAddress());
   ASSERT_EQ(nares.getPort(),na_pred->getPort());
   
   /*- test get successor RPC. -*/
   // set successor.
   DHTKey succ_dhtkey = DHTKey::from_rstring(succ_key);
   NetAddress *na_succ = new NetAddress(succ_addr,succ_port);
   _v1node->setSuccessor(succ_dhtkey,*na_succ);
   
   // test set_predecessor
   _dnode->_l1_client->RPC_getSuccessor(_v1node->getIdKey(),*_na_dnode,
					     dkres,nares,
					     status);
   ASSERT_EQ(DHT_ERR_OK,status);
   ASSERT_EQ(succ_key,dkres.to_rstring());
   ASSERT_EQ(nares.getNetAddress(),na_succ->getNetAddress());
   ASSERT_EQ(nares.getPort(),na_succ->getPort());
      
   //TOTO: test join
   
   
   //TODO: test notify.
   
   //TODO: test find_closest_predecessor.
   status = 0;
   
}

int main(int argc, char **argv)
{
   ::testing::InitGoogleTest(&argc, argv);
   return RUN_ALL_TESTS();
}
