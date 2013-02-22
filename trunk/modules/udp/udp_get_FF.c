/**@file udp_get_ff.c
 * udp_get_ff.c
 *
 *  Created on: Jul 8, 2010
 *      Author: Abdallah Abdallah
 */

/**
 * @brief fetches the FF packet and sends it to the proper function in the UDP module
 *
 * The function first copies the FF into local memory. From there a few checks are made to determine
 * the Fins Frame's destination within the UDP module.
 *
 */
#include "udp.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <finstypes.h>
#include <finsqueue.h>

extern struct udp_statistics udpStat;

