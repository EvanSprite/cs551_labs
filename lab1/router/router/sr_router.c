/**********************************************************************
 * file:  sr_router.c
 * date:  Mon Feb 18 12:50:42 PST 2002
 * Contact: casado@stanford.edu
 *
 * Description:
 *
 * This file contains all the functions that interact directly
 * with the routing table, as well as the main entry method
 * for routing.
 *
 **********************************************************************/

#include <stdio.h>
#include <assert.h>


#include "sr_if.h"
#include "sr_rt.h"
#include "sr_router.h"
#include "sr_protocol.h"
#include "sr_arpcache.h"
#include "sr_utils.h"

/*---------------------------------------------------------------------
 * Method: sr_init(void)
 * Scope:  Global
 *
 * Initialize the routing subsystem
 *
 *---------------------------------------------------------------------*/

void sr_init(struct sr_instance* sr)
{
    /* REQUIRES */
    assert(sr);

    /* Initialize cache and cache cleanup thread */
    sr_arpcache_init(&(sr->cache));

    pthread_attr_init(&(sr->attr));
    pthread_attr_setdetachstate(&(sr->attr), PTHREAD_CREATE_JOINABLE);
    pthread_attr_setscope(&(sr->attr), PTHREAD_SCOPE_SYSTEM);
    pthread_attr_setscope(&(sr->attr), PTHREAD_SCOPE_SYSTEM);
    pthread_t thread;

    pthread_create(&thread, &(sr->attr), sr_arpcache_timeout, sr);
    
    /* Add initialization code here! */

} /* -- sr_init -- */

/*---------------------------------------------------------------------
 * Method: sr_handlepacket(uint8_t* p,char* interface)
 * Scope:  Global
 *
 * This method is called each time the router receives a packet on the
 * interface.  The packet buffer, the packet length and the receiving
 * interface are passed in as parameters. The packet is complete with
 * ethernet headers.
 *
 * Note: Both the packet buffer and the character's memory are handled
 * by sr_vns_comm.c that means do NOT delete either.  Make a copy of the
 * packet instead if you intend to keep it around beyond the scope of
 * the method call.
 *
 *---------------------------------------------------------------------*/

void sr_handlepacket(struct sr_instance* sr,
        uint8_t * packet/* lent */,
        unsigned int len,
        char* interface/* lent */)
{
  /* REQUIRES */
  assert(sr);
  assert(packet);
  assert(interface);

  printf("*** -> Received packet of length %d \n",len);

  /* fill in code here */

  print_hdrs(packet, len);
  int min_eth_hdr_l  = sizeof(sr_ethernet_hdr_t);
  if(len < min_eth_hdr_l){
    fprintf(stderr, "Ethernet Packet is too small for a header !!!\n");
    return;
  }

  uint16_t ethtype = ethertype(packet);

  if(ethtype == ethertype_ip){
    sr_handle_ip_packet(sr, packet, len, interface);
  }else if(ethtype == ethertype_arp){
    sr_handle_arp_packet(sr, (sr_arp_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t)), len - sizeof(sr_ethernet_hdr_t), interface);
  }

}/* end sr_ForwardPacket */

  /* Handle ARP packets. Call recv_arp to store ARP info into the cache. 
   * Then, if it is an ARP request, sends reply, else, return */
  void sr_handle_arp_packet(struct sr_instance* sr, struct sr_arp_hdr * arp_packet, unsigned int len, char* interface){
	  int min_length = sizeof(sr_arp_hdr_t);
	  if (len < min_length) {
		  fprintf(stderr, "ARP Packet is too small !!!\n");
		  return;
	  }

	  struct sr_if* iface = sr_get_interface(sr, interface);
	  if (iface->ip == arp_packet->ar_tip){
		  sr_recv_arp(sr, arp_packet);
		  if (ntohs(arp_packet->ar_op) == arp_op_request){
			  sr_send_arp(sr, arp_op_reply, interface, arp_packet->ar_sha, arp_packet->ar_sip);
		  }
		  else if (ntohs(arp_packet->ar_op) == arp_op_reply){
			  return;
		  }
	  }
	  else{
		  /* ARP is not for me, drop it */
		  fprintf(stderr, "ARP is not for me\n");
	  }
  }

/* Handles an IP packet.
 * First, check whether packet is for me. if is, call sr_handle_my_ip_packet; 
 * if not, decrement TTL and recompute checksum, then forward it. */
void sr_handle_ip_packet(struct sr_instance* sr, uint8_t * packet, unsigned int len, char* interface){
	int min_ip_hdr_l = sizeof(sr_ip_hdr_t) + sizeof(sr_ethernet_hdr_t);
    if (len < min_ip_hdr_l) {
      fprintf(stderr, "IP Packet is too small !!!\n");
      return;
    }

    sr_ip_hdr_t *ip_hdr = (sr_ip_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t));
    unsigned int ip_hdr_l = ip_hdr->ip_hl * 4;

    struct sr_if * iface = sr_check_packet(sr, ip_hdr->ip_dst);
    if(iface){
      fprintf(stderr, "IP packet is for me\n");
      sr_handle_my_ip_packet(sr, ip_hdr);
      return;
    }

     uint8_t new_ttl = ip_hdr->ip_ttl - 1;
     if(new_ttl == 0){
       fprintf(stderr, "TTL hits zero, sending an ICMP packet back....\n");
       uint8_t * buf = calloc(4 + ip_hdr_l + 8, 1);
       memcpy(buf + 4, ip_hdr, ip_hdr_l + 8);
       iface = sr_get_interface(sr, interface);
       sr_send_icmp(sr, icmp_ttl, icmp_ttl_code, iface->ip, ip_hdr->ip_src, buf, 4 + ip_hdr_l + 8);
       free(buf);
       return;
     }

     ip_hdr->ip_ttl = new_ttl;
     ip_hdr->ip_sum = 0;
     ip_hdr->ip_sum = cksum(ip_hdr, ip_hdr_l);

    struct sr_rt * route = longest_prefix_match(sr, ip_hdr->ip_dst);
    if(route){
      uint8_t *temp = malloc(len);
      if(!temp) return;
      
      memcpy(temp, packet, len);
      sr_ethernet_hdr_t *eth_hdr = (sr_ethernet_hdr_t *)temp;
      struct sr_if * node = sr_get_interface(sr, route->interface);
      memcpy(eth_hdr->ether_shost, node->addr, ETHER_ADDR_LEN);

      sr_attempt_send(sr, route->gw.s_addr, temp, len, route->interface);
      free(temp);
    }else{
      fprintf(stderr, "No entry in the routing table matches, send ICMP3 back\n");
      struct sr_if *ifback = sr_get_interface(sr, interface);
      sr_send_icmp3(sr, icmp_unreach, icmp_network_unreach, ifback->ip, ip_hdr->ip_src, (uint8_t*)ip_hdr, ip_hdr_l + 8);
    }
}

/* check if packet is for me, if is, return interface, if not return null*/
struct sr_if * sr_check_packet(struct sr_instance *sr, uint32_t ip_dest){
  struct sr_if * iface = sr->if_list;
  while (iface){
	  if (iface->ip == ip_dest){
		  return iface;
	}
	iface = iface->next;
  }
  return NULL;
}

/* Handle an IP packet for me/router;
  if it's ICMP echo, send reply, if it's TCP or UDP, send port unreachable to sender
*/
void sr_handle_my_ip_packet(struct sr_instance * sr, struct sr_ip_hdr * ip_hdr){

    unsigned int ip_hdr_l = ip_hdr->ip_hl * 4;
    uint8_t * ip_payload = ((uint8_t *)ip_hdr) + ip_hdr_l;
    if(ip_hdr->ip_p == ip_protocol_icmp){
      struct sr_icmp_hdr * icmp_hdr = (sr_icmp_hdr_t *)(ip_payload);
      unsigned int icmp_payload_l = ntohs(ip_hdr->ip_len) - ip_hdr_l - sizeof(sr_icmp_hdr_t);

      if(icmp_hdr->icmp_type == icmp_echo_request){
        sr_send_icmp(sr, icmp_echo_reply, icmp_echo_reply_code, ip_hdr->ip_dst, ip_hdr->ip_src, (uint8_t *)(icmp_hdr+1), icmp_payload_l);  
      }
    }else{
      /* Assuming it's TCP or UDP */
      fprintf(stderr, "Not ICMP packet, port Unreachable !!!\n");
      sr_send_icmp3(sr, icmp_unreach, icmp_port_unreach, ip_hdr->ip_dst, ip_hdr->ip_src, (uint8_t*)ip_hdr, ip_hdr_l + 8);
    }
}

/* Sends an ICMP packet of type 3 */
void sr_send_icmp3(struct sr_instance *sr, enum sr_icmp_type type, enum sr_icmp_code code, uint32_t ip_source, uint32_t ip_dest, uint8_t *data, unsigned int len){

  int icmp_l = sizeof(sr_icmp_t3_hdr_t);
  sr_icmp_t3_hdr_t *icmp = calloc(1, icmp_l);
  memcpy(icmp->data, data, len);
  icmp->icmp_type = type;
  icmp->icmp_code = code;
  icmp->icmp_sum = 0;
  icmp->icmp_sum = cksum(icmp, icmp_l);
  
  sr_send_ip(sr, ip_protocol_icmp, ip_source, ip_dest, (uint8_t *)icmp, icmp_l);
  free(icmp);
}

/* Sends an ICMP packet not type 3 */
void sr_send_icmp(struct sr_instance *sr, enum sr_icmp_type type, enum sr_icmp_code code, uint32_t ip_source, uint32_t ip_dest, uint8_t *buf, unsigned int len){

  int icmp_l = sizeof(sr_icmp_hdr_t) + len;
  sr_icmp_hdr_t *icmp = calloc(icmp_l, 1);
  memcpy(icmp+1, buf, len);
  icmp->icmp_type = type;
  icmp->icmp_code = code;
  icmp->icmp_sum = 0;
  icmp->icmp_sum = cksum(icmp, icmp_l);
  
  sr_send_ip(sr, ip_protocol_icmp, ip_source, ip_dest, (uint8_t *)icmp, icmp_l);
  free(icmp);
}

/* Send an IP packet */
void sr_send_ip(struct sr_instance *sr, enum sr_ip_protocol protocol, uint32_t source, uint32_t dest, uint8_t *buf, unsigned int len){

	struct sr_rt * rt_node = longest_prefix_match(sr, dest);
	if (!rt_node){
		fprintf(stderr, "No entry in routing table matches !!!\n");
		return;
	}

	struct sr_if * if_node = sr_get_interface(sr, rt_node->interface);
	if (!if_node){
		return;
	}

	int ip_len = sizeof(sr_ip_hdr_t);

	sr_ethernet_hdr_t *eth = calloc(sizeof(sr_ethernet_hdr_t)+ip_len + len, sizeof(uint8_t));
	sr_ip_hdr_t *ip = (sr_ip_hdr_t *)(eth + 1);
	memcpy(ip + 1, buf, len);

	ip->ip_v = 4;
	ip->ip_off = htons(IP_DF);
	ip->ip_hl = 5;
	ip->ip_p = protocol;
	ip->ip_src = source;
	ip->ip_dst = dest;
	ip->ip_len = htons(ip_len + len);
	ip->ip_ttl = 64;
	ip->ip_sum = 0;
	ip->ip_sum = cksum(ip, ip_len);

	eth->ether_type = htons(ethertype_ip);
	memcpy(eth->ether_shost, if_node->addr, ETHER_ADDR_LEN);

	sr_attempt_send(sr, rt_node->gw.s_addr, (uint8_t *)eth, sizeof(sr_ethernet_hdr_t)+ip_len + len, if_node->name);
	free(eth);
}



























