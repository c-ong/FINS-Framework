/**@file create_ff.c
 * create_ff.c
 *
 *  Created on: Jul 5, 2010
 *      Author: Abdallah Abdallah
 */
#include "udp.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <finstypes.h>

/**@brief generates and returns a new Fins Frame using the paramters provided
 * @param dataOrCtrl tells whether or not to make an FCF or FDF
 * @param direction 0 for incoming, up the stack: 1 for outgoing, down the stack
 * @param destID the ID of the module that the FF should be sent to
 * @param PDU_length the length of the PDU
 * @param PDU a pointer to where the PDU actually is
 * @param metadata a pointer to the metadata
 */

extern struct udp_statistics udpStat;

struct finsFrame* create_ff(int dataOrCtrl, int direction, int destID, int PDU_length, uint8_t* PDU, metadata *meta) {
	struct finsFrame *ff = (struct finsFrame *) secure_malloc(sizeof(struct finsFrame));

	uint8_t *data;
	data = (uint8_t *) secure_malloc(PDU_length);
	memcpy(data, PDU, PDU_length);

	if (dataOrCtrl == DATA) {
		ff->dataOrCtrl = DATA;
		ff->destinationID.id = destID;
		ff->destinationID.next = NULL;

		ff->dataFrame.directionFlag = direction;
		ff->dataFrame.pduLength = PDU_length;
		ff->dataFrame.pdu = data;
		ff->metaData = meta;
		//	memcpy(&ff.metaData, metadata, MAX_METADATASIZE);
	}

	if (dataOrCtrl == CONTROL) {
		ff->dataOrCtrl = CONTROL;
		ff->destinationID.id = destID;

		ff->destinationID.next = NULL;
		ff->metaData = meta;
		// fill the important FCF data in here
	}
	//print_finsFrame(ff);
	return ff;
}
