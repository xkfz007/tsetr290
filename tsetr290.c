#include <fcntl.h>
#include <unistd.h> 
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <getopt.h>
#include <iconv.h>
#include <netinet/in.h>

#define CRC32_POLY 0x04c11db7L
#define TS_PACKET_SIZE 188
#define MAX_PID 8191
#define READ_ONCE   7
#define byte uint8_t
#define SYSTEM_CLOCK_FREQUENCY 27000000

static uint32_t crc_table[256];

static void make_crc_table(void)
{
  int i, j;
  int already_done = 0;
  uint32_t crc;

  if (already_done)
    return;
  else
    already_done = 1;

  for (i = 0; i < 256; i++)
  {
    crc = i << 24;
    for (j = 0; j < 8; j++)
    {
      if (crc & 0x80000000L)
        crc = (crc << 1) ^ CRC32_POLY;
      else
        crc = ( crc << 1 );
    }
    crc_table[i] = crc;
  }

}

extern uint32_t crc32_block(uint32_t crc, byte *pData, int blk_len)
{
  static int table_made = 0;
  int i, j;

  if (!table_made) make_crc_table();
  
  for (j = 0; j < blk_len; j++)
  {
    i = ((crc >> 24) ^ *pData++) & 0xff;
    crc = (crc << 8) ^ crc_table[i];
  }
  return crc;
}


int64_t ts_timestamp_diff(int64_t t1, int64_t t0, int64_t ovf)
{
        int64_t td; /* t1 - t0 */
        int64_t hovf = ovf >> 1; /* half overflow */

        td = t1 - t0; /* minus */
        td += ((td >=   0) ? 0 : ovf); /* special: get the distance from t0 to t1 */
        td -= ((td <  hovf) ? 0 : ovf); /* special: (distance < hovf) means t1 is latter or bigger */

        return td; /* [-hovf, +hovf) */
}

/*
unsigned long long parse_timestamp(unsigned char *buf)
{
	unsigned long long a1;
	unsigned long long a2;
	unsigned long long a3;
	unsigned long long ts;

	a1 = (buf[0] & 0x0F) >> 1;
	a2 = ((buf[1] << 8) | buf[2]) >> 1;
	a3 = ((buf[3] << 8) | buf[4]) >> 1;
	ts = (a1 << 30) | (a2 << 15) | a3;
	
	return ts;
}
*/
int main(int argc, char *argv[])
{
	int open_file;
	int byte_read;
	int fd_ts;
	int sc;
	int sl;
	unsigned char * data;
	int data_len;
	//unsigned char timestamp[5];
	//int tsid;
	//int vn;
	//int cni;
	//int sn;
	//int lsn;
	//int dl;
	//int pn;
	//byte   *pd;
	int pdl;
	int i;
	int b;
	uint32_t crc = 0;
 	uint32_t check_crc;
	unsigned short pid;
	int sync_error;
	unsigned long long sync_count;
	unsigned long long sync_error_count;
	unsigned long long sync_loss_count;
	int tei;
	int table_id;
	//int prog;
	int pmt_start;
	//int ssi;
	unsigned long long last_pat = 0;
	unsigned long long last_nit = 0;
	unsigned long long last_sdt = 0;
	unsigned long long last_eit = 0;
	unsigned long long last_tdt = 0;
	unsigned long long last_pts[MAX_PID];
	int progindex = 0;
	int 	streams = 0;
	int hascat = 0;
	int hasnit = 0;
	int hassdt = 0;
	int haseit = 0;
	int hastdt = 0;
//	int hasrst = 0;
	unsigned int adaptation_field;
	unsigned char packet[TS_PACKET_SIZE];
	unsigned char pid_cc_table[MAX_PID];	/* PID table for the continuity counter of the TS packets */
	unsigned char repeated_cc_table[MAX_PID];
	unsigned char pid_cc_status[MAX_PID];
	unsigned int programs[MAX_PID];
	unsigned long long streamtypes[MAX_PID];
	unsigned long long streampids[MAX_PID];
	unsigned int total_pids[MAX_PID];
	int pid_count;
	int ispmt;
	unsigned int valid_stream[MAX_PID];
	unsigned long long pcr_pids[MAX_PID];
	unsigned long long cc_error_count;
	unsigned long long position = 1;
	unsigned long long last_pcr[MAX_PID];
	unsigned long long pcrfound[MAX_PID];
	unsigned long long last_pmt[MAX_PID];
	float pcr_jitter;
	float pcr_delta;
	int pcr_delta_error[MAX_PID];
	unsigned int pcr_ext = 0;
	unsigned long long int pcr_base = 0;
	unsigned long long int pid_pcr_table[MAX_PID];	/* PCR table for the TS packets */
	unsigned long long int pid_pcr_index_table[MAX_PID];	/* PCR index table for the TS packets */
	unsigned long long int new_pcr = 0;	
	unsigned long long int new_pcr_index = 0;	
	unsigned long long bitrate;
	//unsigned long long time = 0;

	if (argc >= 3) {
		open_file = 1;
		fd_ts = open(argv[open_file], O_RDONLY);
		if (fd_ts < 0) {
			fprintf(stderr, "Can't find file %s\n", argv[open_file]);
			return 2;
		}
		bitrate = atol(argv[2]);
		if (bitrate == 0) {
			fprintf(stderr, "transport_rate is 0?\n");
			return 2;
		}
	} else {
		fprintf(stderr, "Usage: 'tsetr290 filename.ts bitrate'\n");
		fprintf(stderr, "tsetr290 run a ETR290 report on a mpeg-ts file.\n");
		return 2;
	}

	memset(pid_cc_table, 0x10,  MAX_PID);
	memset(repeated_cc_table, 0,  MAX_PID);
	memset(pid_pcr_table, 0,  MAX_PID*(sizeof(unsigned long long int)));	
	memset(pid_pcr_index_table, 0,  MAX_PID*(sizeof(unsigned long long int)));
	memset(valid_stream, 0,  MAX_PID*(sizeof(int)));
	memset(streamtypes, 0,  MAX_PID*(sizeof(unsigned long long int)));
	memset(streampids, 0,  MAX_PID*(sizeof(unsigned long long int)));
	memset(pcr_pids, 0,  MAX_PID*(sizeof(unsigned long long int)));
	memset(last_pcr, 0,  MAX_PID*(sizeof(unsigned long long int)));
	memset(pcrfound, 0,  MAX_PID*(sizeof(unsigned long long int)));
	memset(last_pmt, 0,  MAX_PID*(sizeof(unsigned long long int)));
	memset(last_pts, 0,  MAX_PID*(sizeof(unsigned long long int)));
	while(1) {
		
		byte_read = 0;
		byte_read = read(fd_ts, packet, TS_PACKET_SIZE);
		if (byte_read < TS_PACKET_SIZE) {
			break;
		}
		
		//pid
		memcpy(&pid, packet + 1, 2);
		pid = ntohs(pid);
		pid = pid & 0x1fff;
		
		if (pid < MAX_PID) {
			
			
			
			tei = (packet[1] >> 7) & 0x01;
			sc = (packet[3] >> 6) & 0x03;
			table_id = packet[5];
			adaptation_field = (packet[3] & 0x30) >> 4;
			sl = ( (packet[6] & 0x0F) << 8) | packet[7];

			if(packet[0] != 0x47) {
				fprintf(stdout, "1.2  - ERROR - Sync_byte_error - Sync byte is 0x%X not 0x47 at packet %lld\n", packet[0],position);
				sync_error_count+=1;			
				if(sync_error>0 && sync_count>5) {
					fprintf(stdout, "1.1  - ERROR - TS_sync_loss - %lld consecutive sync errors at packet %lld\n", sync_loss_count+1, position);
					sync_loss_count++;
				}
				sync_error++;
			}else{
				if(sync_count>=5)sync_error=0;
				sync_count++;
			}

			//if(!sync_error){
				if(pid == 0){
					if(table_id!=0){
						fprintf(stdout,"1.3b - ERROR - PAT_error - table_id is invalid at packet %lld\n",position);
					}else{
						if( ((1504*(position-last_pat))/(bitrate/1000)) > 500){
							fprintf(stdout,"1.3a - ERROR - PAT_error - spacing exceeds 500ms at packet %lld\n",position);
						}
						if(sc != 0){
							fprintf(stdout,"1.3c - ERROR - PAT_error - scrambling controll is not 0 at packet %lld\n",position);
						}
						if(adaptation_field == 0x01 || adaptation_field == 0x11) { 
						 //fprintf(stdout,"ADAPTING  = %X\n", adaptation_field);
						}
						last_pat = position;
						pmt_start = 13;//this could be valible based on ssi?
						pdl = sl+3-8-4; //done like this to show, whats going on (sec length +3) -8? -4(crc)
						progindex = 0;
						while (pdl > 0){
							//prog = (packet[pmt_start] << 8) | packet[pmt_start+1];
							programs[progindex] = (packet[pmt_start+2] & 0x1F) << 8 | packet[pmt_start+3];
							pmt_start= pmt_start + 4;
							pdl = pdl-4;
							progindex++;
						}

						data = packet+5;
						data_len = sl + 3;
						crc = (crc << 8) | data[data_len-4];
						crc = (crc << 8) | data[data_len-3];
						crc = (crc << 8) | data[data_len-2];
						crc = (crc << 8) | data[data_len-1];
						check_crc = crc32_block(0xffffffff,data,data_len);
						if (check_crc != 0){
						    fprintf(stdout, "2.2  - ERROR - CRC_error - Calculated CRC for PAT is %08x, not 00000000  at packet %lld\n",crc, position);
						}
						
					}
				}

			

				for(i=0; i<=progindex-1; i++){
					//fprintf(stderr, "Check PMT prog #%d\n",i);
					if (programs[i] == pid && table_id == 0x02) { 
						//fprintf(stderr, "FOUND PMT %d (%3d) %d %d\n", programs[i],programs[i], pid, position);
						if( ((1504*(position-last_pmt[pid]))/(bitrate/1000)) > 500){
							fprintf(stdout,"1.5a - ERROR - PMT_error - spacing exceeds 500ms for pid %d at packet %lld\n",pid,position);
						}
						//CHECK SCRAMBLING CONTROL
						if(sc != 0){
							fprintf(stdout,"1.5c - ERROR - PMT_error - scrambling controll is not 0 at packet %lld\n",position);
						}
						//GET THE MAP TO MAKE SURE PIDS EXIST

						
						pcr_pids[i] = ((packet[13] & 0x1F) <<8 ) | packet[14];
						//fprintf(stdout, "PCR PID %04x (%3d)\n",pcr_pids[pid],pcr_pids[pid]);
						pdl = ((packet[15] & 0x0F) <<8 ) | packet[16];
						//fprintf(stdout, "DESCRIPTOR LENGTH %04x (%3d)\n",pdl,pdl);
						if (pdl > 0){
							//PMT DESCRIPTORS
							fprintf(stdout, "PMT descriptors present, Not supported, send sample. Exiting.\n");
							//exit(0);
						}
						int es_info_start = 17;
						pdl = sl+3-16-4; //done like this to show, whats going on (sec length +3) -16???? -4(crc)
						streams=0;
						int espdl;
						while(pdl>0){
							//fprintf(stdout, "Length is %d.\n",pdl);
							streamtypes[streams] = packet[es_info_start];
							streampids[streams] =  ((packet[es_info_start+1] & 0x1F) <<8 ) | packet[es_info_start+2];
							//valid_stream[streampids[streams]] = 0;
							espdl = ((packet[es_info_start+3] & 0x0F) <<8 ) | packet[es_info_start+4];
							//fprintf(stdout, "Stream type %d Pid %d ed_info length %d.\n",streamtypes[streams],streampids[streams],espdl);
							pdl = pdl - 5 - espdl;
							es_info_start = es_info_start + 5 + espdl;
							streams++;
						}
						data = packet + 5;
						data_len = sl + 3;
						crc = 0;
						crc = (crc << 8) | data[data_len-4];
						crc = (crc << 8) | data[data_len-3];
						crc = (crc << 8) | data[data_len-2];
						crc = (crc << 8) | data[data_len-1];
						check_crc = crc32_block(0xffffffff,data,data_len);
						if (check_crc != 0)
						{
						    fprintf(stdout, "2.2  - ERROR - CRC_error - Calculated CRC for PMT is %08x, not 00000000 at packet %lld\n",crc, position);
						}


						last_pmt[pid] = position;
					}else if (programs[i] == pid && table_id != 0x02) {
						//TABLE ID FOR PMT IS INVALID pcr_jitter
						fprintf(stdout, "1.5b - ERROR - PMT_error - PMT (%d) table_id is invalid (0x%X) at packet %lld\n", pid,table_id,position);
					}

					
				}
				
				
				
				if (pid_cc_table[pid] == 0x10) { 
					//fprintf(stderr, "New PID found in stream %d\n", pid);
					//total up anything that can go into a pmt besides a null
					if(pid >=32 && pid < 8191){ 
						ispmt = 0;
						for(i=0; i<=progindex-1; i++){
							if(programs[i] == pid)ispmt=1;
						}
						if(!ispmt){
							total_pids[pid_count] = pid;
							pid_count++;
						}
					}
					
				} else {
					if (((pid_cc_table[pid] + 1) % 16) != (packet[3] & 0xF)) {
						if (adaptation_field == 0x0 || adaptation_field == 0x2) { 
							/* reserved, no increment */;
						} else if ((adaptation_field == 0x1) && ((packet[3] & 0x0f) == pid_cc_table[pid]) && (!repeated_cc_table[pid])) { 
							/* double packet accepted only once */
							repeated_cc_table[pid] = 1;
						} else if ((adaptation_field == 0x3) && ((packet[3] & 0x0f) == pid_cc_table[pid]) && (!repeated_cc_table[pid])) { 
							/* double packet accepted only once */
							repeated_cc_table[pid] = 1;
						} else {
							if(pid_cc_status[pid]>1){
								fprintf(stdout, "1.4  - ERROR - Continuity_count_error - pid %d  Expected %d, found %d at packet %lld\n", pid, ((pid_cc_table[pid] + 1) % 0xF), (packet[3] & 0xF),position);
								pid_cc_status[pid]=0;
								cc_error_count++;
							}
							pid_cc_status[pid]++;
						}
					}
				}
				pid_cc_table[pid] = packet[3] & 0xF;
				
				
				for(i=0; i<streams; i++){
					if(streampids[i] == pid){
						valid_stream[pid] = 1;
					}
					
				}
			//}
		
			//Check for the TEI being set.
			if(tei){
				fprintf(stdout, "2.1  - ERROR - Transport_error - Transport_error indicator is 1  on PID %d at packet %lld\n", pid,position);
			}

			//Check for CAT packets
			if(pid == 1){				
				//CAT
				if(table_id!=0x01){
						fprintf(stdout,"2.6b - ERROR - CAT_error - Section with table_id other than 0x01 found on PID 0x0001 at packet %llu\n",position);
				}else{
					hascat = 1;
					data = packet+5;
					data_len = sl + 3;
					crc = 0;
					crc = (crc << 8) | data[data_len-4];
					crc = (crc << 8) | data[data_len-3];
					crc = (crc << 8) | data[data_len-2];
					crc = (crc << 8) | data[data_len-1];
					check_crc = crc32_block(0xffffffff,data,data_len);
					if (check_crc != 0)
					{
						fprintf(stdout, "2.2  - ERROR - CRC_error - Calculated CRC for CAT is %08x, not 00000000  at packet %lld\n",crc, position);
					}
				}
			}
			
			if(sc!=0x00 && !hascat){
				fprintf(stdout,"2.6a - ERROR - CAT_error -  transport_scrambling_control set, but no section with no CAT present at packet %llu\n",position);
			}
			

			//Check for NIT packets
			if(pid == 0x10){
				if((table_id== 0x40 || table_id== 0x41 || table_id== 0x72 )){
					hasnit = 1;
					data = packet+5;
					data_len = sl + 3;
					crc = 0;
					crc = (crc << 8) | data[data_len-4];
					crc = (crc << 8) | data[data_len-3];
					crc = (crc << 8) | data[data_len-2];
					crc = (crc << 8) | data[data_len-1];
					check_crc = crc32_block(0xffffffff,data,data_len);
					if (check_crc != 0)
					{
						fprintf(stdout, "2.2  - ERROR - CRC_error - Calculated CRC for NIT is %08x, not 00000000  at packet %lld\n",crc, position);
					}
					//JUST NIT, NOT ST?
					if((table_id== 0x40 || table_id== 0x41)){
						if( ((1504*(position-last_nit))/(bitrate/1000)) > 10000){
							fprintf(stdout,"3.1b  - ERROR - NIT_error - NIT spacing on pid %d is greater than 10000ms (%lld)  at packet %lld\n",pid,((1504*(position-last_nit))/(bitrate/1000)),position);
						}
						last_nit = position;
					}
				}else{
					fprintf(stdout,"3.1a - ERROR - NIT_error -  Section with table_id other than 0x40 or 0x41 or 0x72 at with pid 0x10 at packet %lld\n",position);
				}
			}
			
			//Check for SDT packets
			if(pid == 0x11 ){

				if( table_id== 0x42){
					hassdt = 1;
					data = packet+5;
					data_len = sl + 3;
					crc = 0;
					crc = (crc << 8) | data[data_len-4];
					crc = (crc << 8) | data[data_len-3];
					crc = (crc << 8) | data[data_len-2];
					crc = (crc << 8) | data[data_len-1];
					check_crc = crc32_block(0xffffffff,data,data_len);
					if (check_crc != 0)
					{
						fprintf(stdout, "2.2  - ERROR - CRC_error - Calculated CRC for SDT is %08x, not 00000000  at packet %lld\n",crc, position);
					}
					if( ((1504*(position-last_sdt))/(bitrate/1000)) > 2000){
							fprintf(stdout,"3.5a  - ERROR - SDT_error - Sections with table_id = 0x42 not present on PID 0x0011 for more than 2 seconds at packet %lld\n",position);
					}
					last_sdt = position;
				}else if( table_id== 0x46 ||table_id== 0x4A || table_id== 0x72){
					hassdt = 1;
					//SDT OTHER, NOT SURE WHAT TO REALLY DO HERE.....  (0x46, 0x4A or 0x72 tables) a4 is bat below...
					last_sdt = position;//Not sure if this counts for timing or not...
					data = packet+5;
					data_len = sl + 3;
					crc = 0;
					crc = (crc << 8) | data[data_len-4];
					crc = (crc << 8) | data[data_len-3];
					crc = (crc << 8) | data[data_len-2];
					crc = (crc << 8) | data[data_len-1];
					check_crc = crc32_block(0xffffffff,data,data_len);
					if (check_crc != 0)
					{
						fprintf(stdout, "2.2  - ERROR - CRC_error - Calculated CRC for BAT is %08x, not 00000000  at packet %lld\n",crc, position);
					}
				}else{
					fprintf(stdout,"3.5b - ERROR - SDT_error -  Section with table_id other than 0x42, 0x46, 0x4A or 0x72 found on PID 0x0011 at packet %lld\n",position);
				}
				//Get the service ID's for fun.
			}



			//Check for TDT packets
			if(pid == 0x14){
				if( table_id== 0x70 ){
					hastdt = 1;
					data = packet+5;
					data_len = sl + 3;
					crc = 0;
					crc = (crc << 8) | data[data_len-4];
					crc = (crc << 8) | data[data_len-3];
					crc = (crc << 8) | data[data_len-2];
					crc = (crc << 8) | data[data_len-1];
					check_crc = crc32_block(0xffffffff,data,data_len);
					if (check_crc != 0)
					{
						fprintf(stdout, "2.2  - ERROR - CRC_error - Calculated CRC for TDT is %08x, not 00000000  at packet %lld\n",crc, position);
					}
					if( ((1504*(position-last_tdt))/(bitrate/1000)) > 30000){
							fprintf(stdout,"3.8a  - ERROR - TDT_error - Sections with table_id = 0x70 not present on PID 0x0014 for more than 30 seconds at packet %lld\n",position);
					}
					last_tdt = position;
				}else if(table_id== 0x73  || table_id== 0x72){ // 72???
					hastdt = 1;
					last_tdt = position;
					data = packet+5;
					data_len = sl + 3;
					crc = 0;
					crc = (crc << 8) | data[data_len-4];
					crc = (crc << 8) | data[data_len-3];
					crc = (crc << 8) | data[data_len-2];
					crc = (crc << 8) | data[data_len-1];
					check_crc = crc32_block(0xffffffff,data,data_len);
					if (check_crc != 0)
					{
						fprintf(stdout, "2.2  - ERROR - CRC_error - Calculated CRC for TOT is %08x, not 00000000  at packet %lld\n",crc, position);
					}
				}else{
					fprintf(stdout,"3.8b - ERROR - TDT_error -  Section with table_id other than 0x70, 0x72, 0x73 found on PID 0x0014 at packet %lld\n",position);
				}
			}
			

			//Check for EIT packets
			if(pid == 0x12){
				if(table_id== 0x4e ){
					haseit = 1;
					data = packet+5;
					data_len = sl + 3;
					crc = 0;
					crc = (crc << 8) | data[data_len-4];
					crc = (crc << 8) | data[data_len-3];
					crc = (crc << 8) | data[data_len-2];
					crc = (crc << 8) | data[data_len-1];
					check_crc = crc32_block(0xffffffff,data,data_len);
					if (check_crc != 0)
					{
						fprintf(stdout, "2.2  - ERROR - CRC_error - Calculated CRC for EIT is %08x, not 00000000  at packet %lld\n",crc, position);
					}
					if( ((1504*(position-last_eit))/(bitrate/1000)) > 2000){
						fprintf(stdout,"3.6a  - ERROR - EIT_error - Sections with table_id = 0x4E not present on PID 0x0012 for more than 2 seconds at packet %lld\n",position);
					}
					last_eit = position;
				}else if((table_id >= 0x4e  && table_id <= 0x6F) || table_id == 0x72){
					haseit = 1;
					last_eit = position; //Again, not sure if these should count towards nit timing?
				}else{
					fprintf(stdout,"3.6b - ERROR - SDT_error -  Sections with table_ids other than in the range 0x4E - 0x6F or 0x72 found on PID 0x0012 at packet %lld\n",position);
				}
			}

			//RST
			if(pid == 0x13){
				if(table_id== 0x71 || table_id==0x72 ){
					//hasrst=1;
				}else{
					fprintf(stdout,"3.7  - ERROR - RST_error -  Sections with table_id other than 0x71 or 0x72 found on PID 0x0013 at packet %lld\n",position);
				
				}
			}
			

			for(i=0; i< progindex; i++){
				if(pcr_delta_error[i] == 0  && ((1504*(position-last_pcr[i]))/(bitrate/1000)) > 100 ){
					if( ((1504*(position-last_pcr[i]))/(bitrate/1000)) > 100 ){
						pcr_delta_error[i] =1; 
					}
				}
			}
			
			for(i=0; i< progindex; i++){
				if(pid == pcr_pids[i]){
					adaptation_field = (packet[3] >> 4) & 0x03;
					if ( (adaptation_field == 3 || adaptation_field == 2) && (packet[5] & 0x10) >> 4) {
						if(pcr_delta_error[i]){
							fprintf(stdout,"2.3a - ERROR - PCR_error - PCR delta is greater than 100ms (%lld)  at packet %lld\n",((1504*(position-last_pcr[i]))/(bitrate/1000)),position);
							pcr_delta_error[i]=0;
						}
						pcrfound[i] = 1;
						if(((1504*(position-last_pcr[i]))/(bitrate/1000)) > 40){
							pcrfound[i] = 0;
						}
						if ((packet[3] & 0x20) && (packet[4] != 0) && (packet[5] & 0x10)) { /* there is a pcr field */
							pcr_base = (((unsigned long long int)packet[6]) << 25) + (packet[7] << 17) + (packet[8] << 9) + (packet[9] << 1) + (packet[10] >> 7);
							pcr_ext = ((packet[10] & 1) << 8) + packet[11];
							if (pid_pcr_table[pid] == 0) { 
								pid_pcr_table[pid] = pcr_base * 300 + pcr_ext;
								pid_pcr_index_table[pid] = (position * TS_PACKET_SIZE);
							} else { 
								new_pcr = pcr_base * 300 + pcr_ext;
								new_pcr_index = (position * TS_PACKET_SIZE);
								
								pcr_jitter =(((float) (new_pcr - pid_pcr_table[pid])) / SYSTEM_CLOCK_FREQUENCY) - (((float)(new_pcr_index - pid_pcr_index_table[pid])) * 8 / bitrate),
								pcr_delta = ((float)((new_pcr - pid_pcr_table[pid]) * 1000)) / SYSTEM_CLOCK_FREQUENCY;

								if(pcr_delta > 40){
									fprintf(stdout,"2.3b - ERROR - PCR_error - PCR delta is greater than 40ms ( %.2f )  at packet %lld\n",pcr_delta,position);
								}

								if(pcr_jitter > 0.05){ //is this right? the whole nano second stuff seems to be throwing me for a loop
									fprintf(stdout,"2.4  - ERROR - PCR_accuracy_error - PCR jitter is greater than +- 500ns at packet %lld\n",position);
								}

								pid_pcr_table[pid] = new_pcr;
								pid_pcr_index_table[pid] = new_pcr_index;

							}
						}
						last_pcr[i] = position;
					}
				}
			}
		}

		if ((packet[4] == 0x00) && (packet[5] == 0x00) && (packet[6] == 0x01)) { 
			//memcpy(timestamp, packet +13, 5);
			//time = parse_timestamp(timestamp);	//Don't need it really
			if( ((1504*(position-last_pts[pid]))/(bitrate/1000)) > 700){
				fprintf(stdout,"2.5  - ERROR - PTS_error - PTS spacing on pid %d is greater than 700ms (%lld)  at packet %lld\n",pid,((1504*(position-last_pts[pid]))/(bitrate/1000)),position);
			}
			last_pts[pid] = position;
		}

		//TODO - I'm no expert, and these are well beyond my range at the moment.
		
		// 3.2 - 3.2 SI_repetition_error Repetition rate of SI tables outside of vspecified limits (25ms)
		/*3.3 Buffer_error TB_buffering_error
overflow of transport buffer (TBn)
TBsys_buffering_error
overflow of transport buffer for system
information (Tbsys)
MB_buffering_error
overflow of multiplexing buffer (MBn) or
if the vbv_delay method is used:
underflow of multiplexing buffer (Mbn)
EB_buffering_error
overflow of elementary stream buffer (EBn) or
if the leak method is used:
underflow of elementary stream buffer (EBn)
though low_delay_flag and
DSM_trick_mode_flag are set to 0
else (vbv_delay method)
underflow of elementary stream buffer (EBn)
B_buffering_error
overflow or underflow of main buffer (Bn)
Bsys_buffering_error
overflow of PSI input buffer (Bsys)*/
	
	
	/*3.9 Empty_buffer_error Transport buffer (TBn) not empty at least
once per second
or
transport buffer for system information
(TBsys) not empty at least once per second
or
if the leak method is used
multiplexing buffer (MBn) not empty at least
once per second

*/



/*3.10 Data_delay_error Delay of data (except still picture video data)
through the TSTD buffers superior to
1 second
or
delay of still picture video data through the
TSTD buffers superior to 60 seconds
*/



		position++;
	}
	for(i=0; i<streams; i++){
		if(valid_stream[streampids[i]] != 1){
			fprintf(stdout, "1.6  - ERROR - PID_error - PID (%lld) found in PMT, not in stream\n", streampids[i]);
		}
	}
	for(i=0; i<pid_count; i++){
		int match;
		for(b=0; b<streams; b++){
			if(total_pids[i] == streampids[i])match=1;
		}
		if(!match)fprintf(stdout, "3.4  - ERROR - PID_error - PID (%d) found in stream, not in PMT\n", total_pids[i]);
	}
	if(!hasnit){
			fprintf(stdout,"3.1b - ERROR - NIT_error - NIT packets not found in stream\n");
	}
	if(!hassdt){
			fprintf(stdout,"3.5a - ERROR - SDT_error - SDT packets not found in stream\n");
	}
	if(!haseit){
			fprintf(stdout,"3.6a - ERROR - EIT_error - EIT packets not found in stream\n");
	}
	if(!hastdt){
			fprintf(stdout,"3.8a - ERROR - TDT_error - TDT packets not found in stream\n");
	}
	return 0;
}