/*
 * V2X_gsm.c
 *
 * Created: 2/12/2016 11:01:08 AM
 *  Author: Jesse Banks
 */

#include "V2X.h"

char imei[16] = "\0";
char latitude[13] = "\0";
char latitude_hemispere[2] = "\0";
char longitude[13] = "\0";
char longitude_hemispere[2] = "\0";

volatile buff GSM;
char stng[100] = "\0";
int GSM_sequence_state = GSM_state_idle;
int GSM_subsequence_state = GSM_subssequence_FAIL;

void GSM_uart_start (void) {
	usart_rs232_options_t usart_cfg = {
		.baudrate =   SIM_BAUDRATE,
		.charlength = SIM_CHAR_LENGTH,
		.paritytype = SIM_PARITY,
		.stopbits =   SIM_STOP_BIT
	};
		
	//start UART
	sysclk_enable_module(SIM_PORT_SYSCLK, SIM_SYSCLK);
	usart_init_rs232(SIM_UART, &usart_cfg);  //returns true if successfull at calculating the baud rate
	usart_set_rx_interrupt_level(SIM_UART, USART_INT_LVL_HI);
	usart_set_tx_interrupt_level(SIM_UART, USART_INT_LVL_OFF);
		
	//initialize buffers
	GSM.input_proc_flag = GSM.input_proc_index = GSM.input_index = 0;
	GSM.output_proc_active_flag = GSM.output_proc_index = 0;
	CTL_add_to_buffer(&GSM, BUFFER_IN, '\n'); //put something in buffer so pointers are different
	CTL_mark_for_processing(&GSM, BUFFER_IN);
}

void GSM_set_tx_int(void) {
	usart_set_tx_interrupt_level(SIM_UART, USART_INT_LVL_HI);
}

void GSM_clear_tx_int(void) {
	usart_set_tx_interrupt_level(SIM_UART, USART_INT_LVL_OFF);
}

void GSM_add_string_to_buffer(Bool in_out, char * to_add) {
	CTL_add_string_to_buffer(&GSM, in_out, to_add);
}

void GSM_mark_for_processing(Bool in_out) {
	CTL_mark_for_processing(&GSM, in_out);
}

void GSM_new_data (uint8_t value) {
	CTL_add_to_buffer(&GSM, BUFFER_IN, value);
	if (value == '\n' || value == '\r' ) {
		GSM.input_proc_flag = true;
	}
}

void GSM_send_data (void) {
	if (CTL_bytes_to_send(&GSM, BUFFER_OUT)) { //if bytes left to send
		usart_putchar(SIM_UART, CTL_next_byte(&GSM, BUFFER_OUT) );
	} else { //clean up
		GSM_clear_tx_int();
		GSM.output_proc_loaded = false; //mark string as sent
	}
}

void GSM_modem_init (void) {
	if (GSM_sequence_state == GSM_state_idle) {
		GSM_sequence_state = GSM_power_check;
		GSM_subsequence_state = GSM_subssequence_1; //move to response state
		char hold[2];
		GSM_control (hold);
	}
}

void GSM_time_job (void) {
	if (GSM_sequence_state == GSM_state_idle) {
		GSM_sequence_state = GSM_state_time_get;
		GSM_subsequence_state = GSM_subssequence_1; //move to response state
		char hold[2];
		GSM_control (hold);
	}
}

void GSM_start_GPS_test (void) {
	if (GSM_sequence_state == GSM_state_idle) {
		GSM_sequence_state = GSM_GPS_evaluation;
		GSM_subsequence_state = GSM_subssequence_1; //move to response state
		char hold[2];
		GSM_control (hold);
	}
}

void GSM_stop_test (void) {
	GSM_sequence_state = GSM_state_idle; //move to response state
}

void GSM_process_buffer (void) {
	if (GSM.output_proc_active_flag) {
		if (GSM.output_proc_loaded) { //output buffer is ready to send
			GSM_set_tx_int();		//set ISR flag
			if (GSM.output_proc_index == 0) {//new command
				GSM_send_data();
			}
		} else {
			CTL_purge_buffer(&GSM, BUFFER_OUT);
		}
	}
	while (GSM.input_proc_flag) {
		CTL_copy_to_proc(&GSM); //copy string from buffer
		if (GSM.input_proc_loaded) { //process string
			menu_send_GSM();
			USB_send_string(USB_CMD, GSM.input_proc_buf);
			menu_send_n_st();
			GSM_control(GSM.input_proc_buf);
			GSM.input_proc_loaded = false;		//input proc buffer has been handled
		}
	}
}

void GSM_control (char * responce_buffer) {
	switch (GSM_sequence_state) {
	case GSM_state_idle:
		break;
	case GSM_power_check:
		GSM_control_check(responce_buffer);
		break;
	case GSM_state_start:
		GSM_control_start(responce_buffer);
		break;
	case GSM_state_time_get:
		GSM_time_sync(responce_buffer);
		break;
	case GSM_GPS_evaluation:
		GSM_test_GPS(responce_buffer);
		break;
	default:
		GSM_sequence_state = GSM_state_idle;
	}
}

void GSM_control_check (char * responce_buffer){
	switch (GSM_subsequence_state) {
	/* This power check is actually not optimal for supporting both board versions
	 * ...In Rev20 the SIM is not the only thing on the 4v rail, so we can't rely
	 * on the operational convention of it being disabled/enabled as a check
	 * for the SIM's absolute state
	 *
	 * Works fine for Rev1.2, needs help on Rev2.0.
	 */
	case GSM_subssequence_1:  //check module power
		USB_tx_string_P(PSTR("\rCTL>>>:Power up GSM\r\n"));  //does not need end of string, exits through menu
		PWR_gsm_start();
		GSM_subsequence_state = GSM_subssequence_3;
		job_set_timeout(SYS_GSM, 20); //give SIM module 10 seconds to start
		break;
#if SIMCOM == SIMCOM_SIM5320A
	/* 7100a does not appear to send the START message, so has real trouble
	 * with this step. This appears to be why just sending the machine to subsequence_3
	 * has been more reliable with the new version
	 */
	case GSM_subssequence_2: //Module clean boot, look for "start"
		if (strcmp_P(responce_buffer, PSTR("START")) == 0) {
			GSM_subsequence_state = GSM_subssequence_3;  //got expected response, go to next step
			GSM_control_check(responce_buffer);
		}
		job_check_fail(SYS_GSM);
		break;
#endif
	case GSM_subssequence_3: //check for SIM power state
		if (sim_power_status()) {
			menu_send_CTL();
			USB_tx_string_P(PSTR("GSM is powered\r\n>"));
			GSM_subsequence_state = GSM_subssequence_6;
			GSM_control_check(responce_buffer);
		} else { //try a reset
			menu_send_CTL();
			USB_tx_string_P(PSTR("GSM rebooting\r\n>"));
			PWR_gsm_start();
			GSM_subsequence_state = GSM_subssequence_3; // Check for power again
			job_set_timeout(SYS_GSM, 20); //give SIM module 20 seconds to start
		}
		break;
	case GSM_subssequence_6: //check for command responce
		CTL_add_string_to_buffer_P(&GSM, BUFFER_OUT, PSTR("AT\r")); //compose message
		CTL_mark_for_processing(&GSM, BUFFER_OUT);
		GSM_subsequence_state = GSM_subssequence_7; //move to response state
		job_set_timeout(SYS_GSM, 2); //give SIM module 2 seconds to restart
		break;
	case GSM_subssequence_7:
		if (strcmp_P(responce_buffer, PSTR("OK")) == 0) {
			menu_send_CTL();
			USB_tx_string_P(PSTR("GSM Responding\r\n>"));
			GSM_subsequence_state = GSM_subssequence_1;  //got expected response, go to next step
			GSM_sequence_state = GSM_state_start;
			GSM_control(responce_buffer); //start next state
		}
		job_check_fail(SYS_GSM);
		break;
	case GSM_subssequence_FAIL:
	default:
		GSM_sequence_state = GSM_state_idle;
		job_clear_timeout(SYS_GSM);
		menu_send_CTL();
		USB_tx_string_P(PSTR("Could not connect to GSM\r\n>"));
		break;
	}
}

void GSM_control_start (char * responce_buffer){
	switch (GSM_subsequence_state) {
	case GSM_subssequence_1:
		CTL_add_string_to_buffer_P(&GSM, BUFFER_OUT, PSTR("ATE0\r")); //compose message
		CTL_mark_for_processing(&GSM, BUFFER_OUT);
		GSM_subsequence_state = GSM_subssequence_2; //move to response state
		job_set_timeout(SYS_GSM, 2);
		break;
	case GSM_subssequence_2:
		if (strcmp_P(responce_buffer, PSTR("OK")) == 0) {
			menu_send_CTL();
			USB_tx_string_P(PSTR("GSM Echo off\r\n>"));
			GSM_subsequence_state = GSM_subssequence_3; //move to response state
			GSM_control(responce_buffer);
		}
		job_check_fail(SYS_GSM);
		break;
	case GSM_subssequence_3:
		CTL_add_string_to_buffer_P(&GSM, BUFFER_OUT, PSTR("ATI\r")); //compose message
		CTL_mark_for_processing(&GSM, BUFFER_OUT); //send it
		GSM_subsequence_state = GSM_subssequence_4; //move to response state
		job_set_timeout(SYS_GSM, 2);
		break;
	case GSM_subssequence_4:  //get device information
		if (strcmp_P(responce_buffer, PSTR("Model: SIMCOM_SIM5320A")) == 0) {
			menu_send_CTL();
			USB_tx_string_P(PSTR("SIM5320A device detected\r\n>"));
			job_set_timeout(SYS_GSM, 2);
			GSM_subsequence_state = GSM_subssequence_5;  //got expected response, go to next step
		} else if (strcmp_P(responce_buffer, PSTR("Model: SIMCOM_SIM7100A")) == 0) { //got new version of simcom chip
			menu_send_CTL();
			USB_tx_string_P(PSTR("SIM7100A device detected\r\n>"));
			job_set_timeout(SYS_GSM, 2);
			GSM_subsequence_state = GSM_subssequence_5;  //got expected response, go to next step
		}	else if (strcmp_P(responce_buffer, PSTR("OK")) == 0) {	//did not see matching device ID
			GSM_subsequence_state = GSM_subssequence_FAIL;
			job_clear_timeout(SYS_GSM);
			GSM_control(responce_buffer);
		}  //else {keep looking}
		job_check_fail(SYS_GSM);
		break;
	case GSM_subssequence_5:
		clear_buffer(stng);
        /* Operational divergence SIMCOM 5320a vs 7100a
		 * 5320a stores prints out "IMEI: ..."
		 * 7100a prints out "IMEISV: ..."
		 */
#if SIMCOM == SIMCOM_SIM5320A
		for (int i = 0; i < 5; i++) {stng[i] = responce_buffer[i];} //move first 4 to compare
		if (strcmp_P(stng, PSTR("IMEI:")) == 0) {
			clear_buffer(imei);
			strcat(imei, responce_buffer+6);
			menu_send_CTL();
			USB_tx_string_P(PSTR("IMEI captured"));
			menu_send_n_st();
			job_set_timeout(SYS_GSM, 12);
			GSM_subsequence_state = GSM_subssequence_6;  //got expected response, go to next step
		}
#elif SIMCOM == SIMCOM_SIM7100A
		for (int i = 0; i < 7; i++) {stng[i] = responce_buffer[i];} //move first 6 to compare
		if (strcmp_P(stng, PSTR("IMEISV:")) == 0) {
			clear_buffer(imei);
			strcat(imei, responce_buffer+8);
			menu_send_CTL();
			USB_tx_string_P(PSTR("IMEISV captured"));
			menu_send_n_st();
			job_set_timeout(SYS_GSM, 12);
			GSM_subsequence_state = GSM_subssequence_6;  //got expected response, go to next step
		}
#endif
		job_check_fail(SYS_GSM);
		break;
	case GSM_subssequence_6:
		if (strcmp_P(responce_buffer, PSTR("PB DONE")) == 0){
			menu_send_CTL();
			USB_tx_string_P(PSTR("GSM Started\r\n>"));
			CTL_add_string_to_buffer_P(&GSM, BUFFER_OUT, PSTR("AT+CGPSAUTO=1\r")); //compose message
			CTL_mark_for_processing(&GSM, BUFFER_OUT); //send it
			GSM_subsequence_state = GSM_subssequence_7;
			job_set_timeout(SYS_GSM, 2);
		}  //else {keep looking}
		job_check_fail(SYS_GSM);
		break;
	case GSM_subssequence_7:
		if (strcmp_P(responce_buffer, PSTR("OK")) == 0){
			menu_send_CTL();
			USB_tx_string_P(PSTR("GPS Started\r\n>"));
			GSM_sequence_state = GSM_state_idle;
			job_clear_timeout(SYS_GSM);
		}  //else {keep looking}
		job_check_fail(SYS_GSM);
		break;
	case GSM_subssequence_FAIL:
	default:
		menu_send_CTL();
		USB_tx_string_P(PSTR("GSM Start failure\r\n>"));
		GSM_sequence_state = GSM_state_idle;
		job_clear_timeout(SYS_GSM);
		break;
	}
}

void GSM_time_sync (char * responce_buffer) {
	switch (GSM_subsequence_state) {
	case GSM_subssequence_1:
		CTL_add_string_to_buffer_P(&GSM, BUFFER_OUT, PSTR("AT+CGPSINFO\r")); //compose message
		CTL_mark_for_processing(&GSM, BUFFER_OUT);
		GSM_subsequence_state = GSM_subssequence_2; //move to response state
		job_set_timeout(SYS_GSM, 2);
		break;
	case GSM_subssequence_2:
		for (int i = 0; i < 10; i++) {stng[i] = responce_buffer[i];} //move first 10 to compare
		// format for this string is altered in simcom version
		// 5320a: "+CGPSINFO:,,,,,,,,"  == [10] ','
		// 7100a: "+CGPSINFO: ,,,,,,,," == [11] ','
#if SIMCOM == SIMCOM_SIM5320A
		if (strcmp_P(stng, PSTR("+CGPSINFO:")) == 0 && responce_buffer[10] != ',') {
#elif SIMCOM == SIMCOM_SIM7100A
		if (strcmp_P(stng, PSTR("+CGPSINFO:")) == 0 && responce_buffer[11] != ',') {
#endif
			GSM_parse_gps_info(responce_buffer);
			GSM_subsequence_state = GSM_subssequence_3;
			job_set_timeout(SYS_GSM, 2);
		}
		job_check_fail(SYS_GSM);
		break;
	case GSM_subssequence_3:
		if (strcmp_P(responce_buffer, PSTR("OK")) == 0) {
			GSM_sequence_state = GSM_state_idle;

			job_clear_timeout(SYS_GSM);
		}
		job_check_fail(SYS_GSM);
		break;
	case GSM_subssequence_FAIL:
	default:
		menu_send_CTL();
		USB_tx_string_P(PSTR("GPS time update FAIL\r\n>"));
		GSM_sequence_state = GSM_state_idle;
		job_clear_timeout(SYS_GSM);
		break;
	}
}

void show_buffer(char * buffer) {
	USB_tx_string_P(PSTR("\""));
	USB_send_string(USB_CMD, buffer);
	USB_tx_string_P(PSTR("\""));

}

void GSM_parse_gps_info (char * responce_buffer) {

	char * start_ptr = strchr(responce_buffer, ':') + 1;
	for (int i = 0; start_ptr[i] != ','; i++) {
		latitude[i] = start_ptr[i];	//copy lat string
	}
	start_ptr = strchr(start_ptr, ',') + 1;
	for (int i = 0; start_ptr[i] != ','; i++) {
		latitude_hemispere[i] = start_ptr[i];	//copy lat hemi string
	}
	start_ptr = strchr(start_ptr, ',') + 1;
	for (int i = 0; start_ptr[i] != ','; i++) {
		longitude[i] = start_ptr[i];	//copy long string
	}
	start_ptr = strchr(start_ptr, ',') + 1;
	for (int i = 0; start_ptr[i] != ','; i++) {
		longitude_hemispere[i] = start_ptr[i];	//copy long hemi string
	}
	start_ptr = strchr(start_ptr, ',') + 1;
	char date[10] = "\0";
	for (int i = 0; start_ptr[i] != ','; i++) {
		date[i] = start_ptr[i];	//copy date string
	}
	start_ptr = strchr(start_ptr, ',') + 1;
	char time[10] = "\0";
	for (int i = 0; start_ptr[i] != ','; i++) {
		time[i] = start_ptr[i];	//copy time string
	}

	if (atoi(date) != 0) { //this logic wont work on Jan 1 2100
		time_set_by_strings(date, time);
		menu_send_CTL();
		USB_tx_string_P(PSTR("GPS time sync @ "));
		time_print_human_readable();
		menu_send_n_st();
	}
}

void GSM_control_fail (void) {
	GSM_subsequence_state = GSM_subssequence_FAIL;
}

char * GSM_get_imei (void) {
	return &imei;
}

void GSM_command_power_off(void) {
	CTL_add_string_to_buffer_P(&GSM, BUFFER_OUT, PSTR("AT+CPOF\r")); //compose message
	CTL_mark_for_processing(&GSM, BUFFER_OUT);
}

void GSM_command_enable_gps_auto(int enable) {
	switch(enable) {
		case 0:
		CTL_add_string_to_buffer_P(&GSM, BUFFER_OUT, PSTR("AT+CGPSAUTO=0\r")); //compose message
		break;
		
		case 1:
		CTL_add_string_to_buffer_P(&GSM, BUFFER_OUT, PSTR("AT+CGPSAUTO=1\r")); //compose message
		break;
		
		default:
		break;
	}
}

void GSM_test_GPS (char * responce_buffer) {
	static int loopCnt;  //tracks long term counts
	static long startTime;
	static uint8_t spinner = 0;
	switch (GSM_subsequence_state) {
	case GSM_subssequence_1:
		ACL_set_sample_off();//disable the accelerometer, using this path for test results
		GSM_subsequence_state = GSM_subssequence_10; //move to start state
		job_set_timeout(SYS_GSM, 2);
		break;
	case GSM_subssequence_10:	
		USB_tx_string_P(PSTR("Open ACL comm path for test results\r\n>"));// SIM module interaction will spam the CMD path so data is sent to the ACL
		USB_send_string(USB_ACL, "GPS test under way\r");
		GSM_subsequence_state = GSM_subssequence_2; //move to start state
		job_set_timeout(SYS_GSM, 1);
		break;
	case GSM_subssequence_2:
		//turn off GPS
//		USB_send_char(USB_ACL, '2');	
		CTL_add_string_to_buffer_P(&GSM, BUFFER_OUT, PSTR("AT+CGPS=0\r")); 
		CTL_mark_for_processing(&GSM, BUFFER_OUT);
		//move to response state
		GSM_subsequence_state = GSM_subssequence_7; 
		job_set_timeout(SYS_GSM, 2);  
		break;
	case GSM_subssequence_3:
//		USB_send_char(USB_ACL, '3');
		menu_send_CTL();
		CTL_add_string_to_buffer_P(&GSM, BUFFER_OUT, PSTR("AT+CGPSCOLD\r")); //compose message
		CTL_mark_for_processing(&GSM, BUFFER_OUT);		
		GSM_subsequence_state = GSM_subssequence_4; //move to response state
		job_set_timeout(SYS_GSM, 2);  //test after 1 second
		break;
	case GSM_subssequence_4: //enters after cold start, confirm accepted command
//		USB_send_char(USB_ACL, '4');
		if (strcmp_P(responce_buffer, PSTR("OK")) == 0) {// cold start accepted
			startTime = time_get();//begin lock tracking
			menu_send_CTL();
			USB_tx_string_P(PSTR("GPS Cold Start @"));
			menu_print_int(startTime);
			menu_send_n_st();
			GSM_subsequence_state = GSM_subssequence_6;
			job_set_timeout(SYS_GSM, 1);  //test after 1 second
		}
		if (strcmp_P(responce_buffer, PSTR("ERROR")) == 0) {// cold start accepted
			menu_send_CTL();
			USB_tx_string_P(PSTR("ERROR, reissue cold start command"));
			menu_send_n_st();
			GSM_subsequence_state = GSM_subssequence_2;
			job_set_timeout(SYS_GSM, 1);  //test after 1 second
		}
		job_check_fail(SYS_GSM);
		break;
	case GSM_subssequence_5: // look for GPS lock
//		USB_send_char(USB_ACL, '5');
		
		loopCnt = startTime - time_get();
		if ((responce_buffer[0] == '+')) { //kinda test for +CGPSINFO: string beginning
			//found GPS info string
			
//			USB_send_char(USB_ACL, '*');
			if  (responce_buffer[12] == ',') {
//				USB_send_char(USB_ACL, ',');
				// no lock, 
				if (loopCnt < GPS_TEST_TIMEOUT) { //if not timeout
					//USB_send_string(USB_ACL, "\nInfo Received, no lock");
				} else { //timeout
					USB_send_string(USB_ACL, "\rAcquisition failed");
				}
			} else { // GPS lock found
				USB_send_char(USB_ACL, 8); //backspace to remove spinner
				USB_send_string(USB_ACL, "Acquisition time: ");
				//long finalTime = startTime - time_get();
				char c_buf[13];
				//ltoa(finalTime, c_buf, 10);
				ltoa( (time_get() - startTime ), c_buf, 10);
				int i = 0;  //clear the pointer
				while (c_buf[i] != 0)
					{USB_send_char(USB_ACL, c_buf[i++]);}
				USB_send_char(USB_ACL, '\r');
				GSM_subsequence_state = GSM_subssequence_8; //restart sequence
			}
		} else {//some other responce from the SIM
			if (strcmp_P(responce_buffer, PSTR("OK")) == 0) {// GPS responce finished
				if (loopCnt < GPS_TEST_TIMEOUT) {
// 					USB_send_char(USB_ACL, '-');
// 					if (loopCnt % 80 == 0) {USB_send_string(USB_ACL, "\n");}
					switch (spinner) {
					case 0:
						USB_send_char(USB_ACL, 8); //backspace to remove spinner
						USB_send_char(USB_ACL, '|'); //this version of spinner
						spinner++;
						break;
					case 1:
						USB_send_char(USB_ACL, 8);
						USB_send_char(USB_ACL, '\\');
						spinner++;
						break;
					case 2:
						USB_send_char(USB_ACL, 8);
						USB_send_char(USB_ACL, '-');
						spinner++;
						break;
					default:
					case 3:	
						USB_send_char(USB_ACL, 8);
						USB_send_char(USB_ACL, '/');
						spinner = 0;
						break;
					}
					GSM_subsequence_state = GSM_subssequence_6; //ask again
				} else{
					GSM_subsequence_state = GSM_subssequence_8; //restart sequence
				}
				
			}
		}	
		job_check_fail(SYS_GSM);
		break;
	case GSM_subssequence_6: // should have been called by timeout
//		USB_send_char(USB_ACL, '6');
		CTL_add_string_to_buffer_P(&GSM, BUFFER_OUT, PSTR("AT+CGPSINFO\r")); //query GPS info
		CTL_mark_for_processing(&GSM, BUFFER_OUT);
		GSM_subsequence_state = GSM_subssequence_5; //ask again
		job_set_timeout(SYS_GSM, 1);
		break;
	case GSM_subssequence_7: // delay
//		USB_send_char(USB_ACL, '7');
		if (strcmp_P(responce_buffer, PSTR("OK")) == 0) {
			menu_send_CTL();
			USB_tx_string_P(PSTR("GPS stopped"));
			menu_send_n_st();
			GSM_subsequence_state = GSM_subssequence_3; //ask again
			job_set_timeout(SYS_GSM, 1);
		}
		job_check_fail(SYS_GSM);
		break;
	case GSM_subssequence_8: // delay
//		USB_send_char(USB_ACL, '8');
			menu_send_CTL();
			USB_tx_string_P(PSTR("GPS test complete\n"));
			menu_send_n_st();
			GSM_subsequence_state = GSM_subssequence_2; //ask again
			job_set_timeout(SYS_GSM, 1);
		break;
	case GSM_subssequence_FAIL:
	default:
//		USB_send_char(USB_ACL, 'F');
		menu_send_CTL();
		USB_tx_string_P(PSTR("GPS test failed, retry\r\n>"));
		GSM_subsequence_state = GSM_subssequence_8;
		GSM_control(responce_buffer);
		break;
	}
	
}

