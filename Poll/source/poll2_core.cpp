/** \file poll2_core.cpp
  * 
  * \brief Controls the poll2 command interpreter and data acquisition system
  * 
  * The Poll class is used to control the command interpreter
  * and data acqusition systems. Command input and the command
  * line interface of poll2 are handled by the external library
  * CTerminal. Pixie16 data acquisition is handled by interfacing
  * with the PixieInterface library.
  *
  * \author Cory R. Thornsberry
  * 
  * \date June 26th, 2015
  * 
  * \version 1.3.01
*/

#include <algorithm>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <sstream>
#include <ctime>

#include <cmath>

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#include "poll2_core.h"
#include "poll2_socket.h"
#include "poll2_stats.h"

#include "CTerminal.h"

// Interface for the PIXIE-16
#include "PixieSupport.h"
#include "Utility.h"
#include "Display.h"

#include "MCA_ROOT.h"
#include "MCA_DAMM.h"

// Values associated with the minimum timing between pixie calls (in us)
// Adjusted to help alleviate the issue with data corruption
#define POLL_TRIES 100

/// 4 GB. Maximum allowable .ldf file size in bytes
#define MAX_FILE_SIZE 4294967296ll

std::vector<std::string> chan_params = {"TRIGGER_RISETIME", "TRIGGER_FLATTOP", "TRIGGER_THRESHOLD", "ENERGY_RISETIME", "ENERGY_FLATTOP", "TAU", "TRACE_LENGTH",
									 "TRACE_DELAY", "VOFFSET", "XDT", "BASELINE_PERCENT", "EMIN", "BINFACTOR", "CHANNEL_CSRA", "CHANNEL_CSRB", "BLCUT",
									 "ExternDelayLen", "ExtTrigStretch", "ChanTrigStretch", "FtrigoutDelay", "FASTTRIGBACKLEN"};

std::vector<std::string> mod_params = {"MODULE_CSRA", "MODULE_CSRB", "MODULE_FORMAT", "MAX_EVENTS", "SYNCH_WAIT", "IN_SYNCH", "SLOW_FILTER_RANGE",
									"FAST_FILTER_RANGE", "MODULE_NUMBER", "TrigConfig0", "TrigConfig1", "TrigConfig2","TrigConfig3","SLOW_FILTER_RANGE"};

MCA_args::MCA_args(){ Zero(); }
	
MCA_args::MCA_args(bool useRoot_, int totalTime_, std::string basename_){
	useRoot = useRoot_;
	totalTime = totalTime_;
	basename = basename_;
}

void MCA_args::Zero(){
	useRoot = false;
	totalTime = 0; // Needs to be zero for checking of MCA arguments
	basename = "MCA";
}

Poll::Poll(){
	pif = new PixieInterface("pixie.cfg");

	clock_vsn = 1000;

	// System flags and variables
	sys_message_head = " POLL2: ";
	kill_all = false; // Set to true when the program is exiting
	start_acq = false; // Set to true when the command is given to start a run
	stop_acq = false; // Set to true when the command is given to stop a run
	record_data = false; // Set to true if data is to be recorded to disk
	do_reboot = false; // Set to true when the user tells POLL to reboot PIXIE
	force_spill = false; // Force poll2 to dump the current data spill
	acq_running = false; // Set to true when run_command is recieving data from PIXIE
	run_ctrl_exit = false; // Set to true when run_command exits
	had_error = false; //Set to true when aborting due to an error.
	file_open = false; //Set to true when a file is opened.
	do_MCA_run = false; // Set to true when the "mca" command is received
	raw_time = 0;

	// Run control variables
	boot_fast = false;
	insert_wall_clock = true;
	is_quiet = false;
	send_alarm = false;
	show_module_rates = false;
	zero_clocks = false;
	debug_mode = false;
	shm_mode = false;
	init = false;

	// Options relating to output data file
	output_directory = "./"; // Set with 'fdir' command
	output_title = "PIXIE data file"; // Set with 'title' command
	next_run_num = 1; // Set with 'runnum' command
	output_format = 0; // Set with 'oform' command

	// The main output data file and related variables
	current_file_num = 0;
	filename_prefix = "run";
	
	statsHandler = NULL;
	
	client = new Client();
}

bool Poll::initialize(){
	if(init){ return false; }

	// Set debug mode
	if(debug_mode){ 
		std::cout << sys_message_head << "Setting debug mode\n";
		output_file.SetDebugMode(); 
	}

	// Initialize the pixie interface and boot
	pif->GetSlots();
	if(!pif->Init()){ return false; }

	if(boot_fast){
		if(!pif->Boot(PixieInterface::DownloadParameters | PixieInterface::SetDAC | PixieInterface::ProgramFPGA)){ return false; } 
	}
	else{
		if(!pif->Boot(PixieInterface::BootAll)){ return false; }
	}

	// Check the scheduler
	Display::LeaderPrint("Checking scheduler");
	int startScheduler = sched_getscheduler(0);
	if(startScheduler == SCHED_BATCH){ std::cout << Display::InfoStr("BATCH") << std::endl; }
	else if(startScheduler == SCHED_OTHER){ std::cout << Display::InfoStr("STANDARD") << std::endl; }
	else{ std::cout << Display::WarningStr("UNEXPECTED") << std::endl; }

	if(!synch_mods()){ return false; }

	// Allocate memory buffers for FIFO
	n_cards = pif->GetNumberCards();
	
	// Two extra words to store size of data block and module number
	std::cout << "\nAllocating memory to store FIFO data (" << sizeof(word_t) * (EXTERNAL_FIFO_LENGTH + 2) * n_cards / 1024 << " kB)" << std::endl;
	
	client->Init("127.0.0.1", 5555);

	return init = true;
}

Poll::~Poll(){
	if(init){
		close();
	}
}

bool Poll::close(){
	if(!init){ return false; }
	
	client->SendMessage((char *)"$KILL_SOCKET", 13);
	client->Close();
	
	// Just to be safe
	if(output_file.IsOpen()){ close_output_file(); }

	delete pif;
	
	init = false;
	
	return true;
}

/* Safely close current data file if one is open. */
bool Poll::close_output_file(bool continueRun /*=false*/){
	file_open = false;

	if(output_file.IsOpen()){ // A file is already open and must be closed
		std::cout << sys_message_head << "Closing output file.\n";
		client->SendMessage((char *)"$CLOSE_FILE", 12);
		output_file.CloseFile((float)statsHandler->GetTotalTime());
		
		if (!continueRun) {
			statsHandler->Clear(); //Clear the stats
			output_file.GetNextFileName(next_run_num,filename_prefix,output_directory); //We call get next file name to update the run number.
		}

		return true;
	}
	std::cout << sys_message_head << "No file is open.\n";
	return true;
}

/**Opens a new file if no file is currently open. The new file is 
 * determined from the output directory, run number and prefix. The run 
 * number may be interated foreward if a file already exists. 
 * If this is a continuation run the run number is not iterated and 
 * instead a suffix number is incremented.
 *
 * \param [in] continueRun Flag indicating that this file should be a continuation run and that the run number should not be iterated.
 */
bool Poll::open_output_file(bool continueRun){
	if(!output_file.IsOpen()){ 
		if(!output_file.OpenNewFile(output_title, next_run_num, filename_prefix, output_directory, continueRun)){
			std::cout << sys_message_head << "Failed to open output file! Check that the path is correct.\n";
			record_data = false;
			return false;
		}

		//Clear the stats
		statsHandler->Clear();

		std::cout << sys_message_head << "Opening output file '" << output_file.GetCurrentFilename() << "'.\n";
		client->SendMessage((char *)"$OPEN_FILE", 12);
	}
	else{ 
		std::cout << sys_message_head << "Warning! A file is already open. Close the current file before opening a new one.\n"; 
		return false;
	}

	file_open = true;
	
	return true;
}

bool Poll::synch_mods(){
	static bool firstTime = true;
	static char synchString[] = "IN_SYNCH";
	static char waitString[] = "SYNCH_WAIT";

	bool hadError = false;
	Display::LeaderPrint("Synchronizing");

	if(firstTime){
		// only need to set this in the first module once
		if(!pif->WriteSglModPar(waitString, 1, 0)){ hadError = true; }
		firstTime = false;
	}
	
	for(unsigned int mod = 0; mod < pif->GetNumberCards(); mod++){
		if (!pif->WriteSglModPar(synchString, 0, mod)){ hadError = true; }
	}

	if (!hadError){ std::cout << Display::OkayStr() << std::endl; }
	else{ std::cout << Display::ErrorStr() << std::endl; }

	return !hadError;
}

void Poll::broadcast_data(word_t *data, unsigned int nWords) {
	if(shm_mode){ // Broadcast the spill onto the network
		char shm_data[40008]; // 40 kB packets of data
		char *data_l = (char *)data; // Local pointer to the data array
		unsigned int num_net_chunks = nWords / 10000;
		unsigned int num_net_remain = nWords % 10000;
		if(num_net_remain != 0){ num_net_chunks++; }
		
		unsigned int net_chunk = 1;
		unsigned int words_bcast = 0;
		if(debug_mode){ std::cout << " debug: Splitting " << nWords << " words into network spill of " << num_net_chunks << " chunks (fragment = " << num_net_remain << " words)\n"; }
		
		while(words_bcast < nWords){
			if(nWords - words_bcast > 10000){ // Broadcast the spill chunks
				memcpy(&shm_data[0], (char *)&net_chunk, 4);
				memcpy(&shm_data[4], (char *)&num_net_chunks, 4);
				memcpy(&shm_data[8], &data_l[words_bcast*4], 40000);
				client->SendMessage(shm_data, 40008);
				words_bcast += 10000;
			}
			else{ // Broadcast the spill remainder
				memcpy(&shm_data[0], (char *)&net_chunk, 4);
				memcpy(&shm_data[4], (char *)&num_net_chunks, 4);
				memcpy(&shm_data[8], &data_l[words_bcast*4], (nWords-words_bcast)*4);
				client->SendMessage(shm_data, (nWords - words_bcast + 2)*4);
				words_bcast += nWords-words_bcast;
			}
			net_chunk++;
		}
	}
	else{ // Broadcast a spill notification to the network
		char *packet = NULL;
		int packet_size = output_file.BuildPacket(packet);
		client->SendMessage(packet, packet_size);
		delete[] packet;
	}
}

int Poll::write_data(word_t *data, unsigned int nWords){
	// Open an output file if needed
	if(!output_file.IsOpen()){
		std::cout << Display::ErrorStr() << " Recording data, but no file is open! Opening a new file.\n";
		open_output_file();
	}

	// Handle the writing of buffers to the file
	std::streampos current_filesize = output_file.GetFilesize();
	if(current_filesize + (std::streampos)(4*nWords + 65552) > MAX_FILE_SIZE){
		// Adding nWords plus 2 EOF buffers to the file will push it over MAX_FILE_SIZE.
		// Open a new output file instead
		std::cout << sys_message_head << "Current filesize is " << current_filesize + (std::streampos)65552 << " bytes.\n";
		std::cout << sys_message_head << "Opening new file.\n";
		close_output_file(true);
		open_output_file(true);
	}

	if (!is_quiet) std::cout << "Writing " << nWords << " words.\n";

	return output_file.Write((char*)data, nWords);
}

/* Print help dialogue for POLL options. */
void Poll::help(){
	std::cout << "  Help:\n";
	std::cout << "   run              - Start data acquisition and start recording data to disk\n";
	std::cout << "   stop             - Stop data acqusition and stop recording data to disk\n";	
	std::cout << "   startacq         - Start data acquisition\n";
	std::cout << "   stopacq          - Stop data acquisition\n";
	std::cout << "   acq (shm)        - Run in \"shared-memory\" mode\n";
	std::cout << "   spill (hup)      - Force dump of current spill\n";
	std::cout << "   prefix [name]    - Set the output filename prefix (default='run_#.ldf')\n";
	std::cout << "   fdir [path]      - Set the output file directory (default='./')\n";
	std::cout << "   title [runTitle] - Set the title of the current run (default='PIXIE Data File)\n";
	std::cout << "   facility [name]  - Set the name of the facility (only for pld output format)\n";
	std::cout << "   runnum [number]  - Set the number of the current run (default=0)\n";
	std::cout << "   oform [0|1|2]    - Set the format of the output file (default=0)\n";
	std::cout << "   close (clo)      - Safely close the current data output file\n";
	std::cout << "   reboot           - Reboot PIXIE crate\n";
	std::cout << "   stats [time]     - Set the time delay between statistics dumps (default=-1)\n";
	std::cout << "   mca [root|damm] [time] [filename] - Use MCA to record data for debugging purposes\n";
	std::cout << "   dump [filename]                   - Dump pixie settings to file (default='Fallback.set')\n";
	std::cout << "   pread [mod] [chan] [param]        - Read parameters from individual PIXIE channels\n";
	std::cout << "   pmread [mod] [param]              - Read parameters from PIXIE modules\n";
	std::cout << "   pwrite [mod] [chan] [param] [val] - Write parameters to individual PIXIE channels\n";
	std::cout << "   pmwrite [mod] [param] [val]       - Write parameters to PIXIE modules\n";
	std::cout << "   adjust_offsets [module]           - Adjusts the baselines of a pixie module\n";
	std::cout << "   find_tau [module] [channel]       - Finds the decay constant for an active pixie channel\n";
	std::cout << "   toggle [module] [channel] [bit]   - Toggle any of the 19 CHANNEL_CSRA bits for a pixie channel\n";
	std::cout << "   toggle_bit [mod] [chan] [param] [bit] - Toggle any bit of any parameter of 32 bits or less\n";
	std::cout << "   csr_test [number]                 - Output the CSRA parameters for a given integer\n";
	std::cout << "   bit_test [num_bits] [number]      - Display active bits in a given integer up to 32 bits long\n";
	std::cout << "   status           - Display system status information\n";
	std::cout << "   debug            - Toggle debug mode flag (default=false)\n";
	std::cout << "   quiet            - Toggle quiet mode flag (default=false)\n";
	std::cout << "   reboot           - Reboot PIXIE crate\n";
	std::cout << "   quit             - Close the program\n";
	std::cout << "   help (h)         - Display this dialogue\n";
	std::cout << "   version (v)      - Display Poll2 version information\n";
}

std::vector<std::string> Poll::TabComplete(std::string cmd) {
	static std::vector<std::string> commands = {"start","startacq","stop","stopacq","prefix","runnum","runtitle","close","pread","pwrite","pmwrite","pmread","status",
												"help","version","shm","spill","hup","fdir","reboot","mca","dump","adjust_offsets","find_tau","toggle","toggle_bit",
												"csr_test","bit_test","debug","quiet","quit","oform","title","facility"};

	std::vector<std::string> matches;
	
	//If we have no space then we are auto completing a command.	
	if (cmd.find(" ") == std::string::npos) {
		for (auto it=commands.begin(); it!=commands.end();++it) {
			if ((*it).find(cmd) == 0) {
				matches.push_back((*it).substr(cmd.length()));
			}
		}
	}
	else {
		//Get the trailing str part to complete
		std::string strToComplete = cmd.substr(cmd.find_last_of(" ")+1);

		//If the inital command is pwrite or pread we try to auto complete the param names
		if (cmd.find("pwrite") == 0 || cmd.find("pread") == 0) {
			for (auto it=chan_params.begin(); it!=chan_params.end();++it) {
				if ((*it).find(strToComplete) == 0) 
					matches.push_back((*it).substr(strToComplete.length()));
			}
		}

		//If the inital command is pmwrite or pmread we try to auto complete the param names
		if (cmd.find("pmwrite") == 0 || cmd.find("pmread") == 0) {
			for (auto it=mod_params.begin(); it!=mod_params.end();++it) {
				if ((*it).find(strToComplete) == 0) 
					matches.push_back((*it).substr(strToComplete.length()));
			}
		}

	}

	return matches; 
}

/* Print help dialogue for reading/writing pixie channel parameters. */
void Poll::pchan_help(){
	std::cout << "  Valid Pixie16 channel parameters:\n";
	for(unsigned int i = 0; i < chan_params.size(); i++){
		std::cout << "   " << chan_params[i] << "\n";
	}
}

/* Print help dialogue for reading/writing pixie module parameters. */
void Poll::pmod_help(){
	std::cout << "  Valid Pixie16 module parameters:\n";
	for(unsigned int i = 0; i < mod_params.size(); i++){
		std::cout << "   " << mod_params[i] << "\n";
	}
}

/**Starts a data recording run. Open data file is closed, the run number is iterated and a new file is opened.
 * If the file was successfully opened the acquisition is started.
 * If a run is already started a warning is displayed and the process is stopped.
 *
 * \return Returns true if successfully starts a run.
 */
bool Poll::StartRun() {
	if(do_MCA_run){
		std::cout << sys_message_head << "Warning! Cannot run acquisition while MCA program is running\n";
		return false;
	}
	else if(acq_running){ 
		std::cout << sys_message_head << "Acquisition is already running\n";
		return false;
	}

	//Close a file if open
	if(output_file.IsOpen()){ close_output_file();	}

	//Preapre the output file
	if (!open_output_file()) return false;
	record_data = true;

	//Start the acquistion
	StartAcq();

	return true;
}

/**Current run is stopped. This includes disabling data recording.
 * This command stops the acquisition even if data recording is not active.
 *
 * \return Returns true if successful.
 */
bool Poll::StopRun() {
	if(!acq_running){ 
		std::cout << sys_message_head << "Acquisition is not running\n"; 
		return false;
	}

	StopAcq();
	
	if (record_data) {
		std::stringstream output;
		output << "Run " << output_file.GetRunNumber() << " time";
		Display::LeaderPrint(output.str());
		std::cout << statsHandler->GetTotalTime() << "s\n";
	}

	record_data = false;
	return true;
}

/**Starts data acquistion. The process then waits until the acquistion is running.
 *	
 *	\return Returns true if successful.
 */
bool Poll::StartAcq() {
	if(do_MCA_run){ 
		std::cout << sys_message_head << "Warning! Cannot run acquisition while MCA program is running\n"; 
		return false;
	}
	else if (acq_running) { 
		std::cout << sys_message_head << "Acquisition is already running\n"; 
		return false;
	}

	//Set start acq flag to be intercepted by run control.
	start_acq = true;

	return true;
}

/**Stops data acquistion. The process waits until the acquistion is stopped.
 * 
 * \return Returns true if succesful.
 */
bool Poll::StopAcq() {
	if(!acq_running){ 
		std::cout << sys_message_head << "Acquisition is not running\n"; 
		return false;
	}

	//Set stop_acq flag to be intercepted by run control.
	stop_acq = true;

	return true;
}


///////////////////////////////////////////////////////////////////////////////
// Poll::command_control
///////////////////////////////////////////////////////////////////////////////

/* Function to control the POLL command line interface */
void Poll::command_control(){
	std::string cmd = "", arg;

	bool cmd_ready = true;
	
	while(true){
		cmd = poll_term_->GetCommand();
		if(cmd == "CTRL_D"){ cmd = "quit"; }
		else if(cmd == "CTRL_C"){ continue; }		
		if (cmd.find("\t") != std::string::npos) {
			poll_term_->TabComplete(TabComplete(cmd.substr(0,cmd.length()-1)));
			continue;
		}
		poll_term_->flush();
		//poll_term_->print((cmd+"\n").c_str()); // This will force a write before the cout stream dumps to the screen

		if(cmd_ready){			
			if(cmd == ""){ continue; }
			
			size_t index = cmd.find(" ");
			if(index != std::string::npos){
				arg = cmd.substr(index+1, cmd.size()-index); // Get the argument from the full input string
				cmd = cmd.substr(0, index); // Get the command from the full input string
			}
			else{ arg = ""; }

			std::vector<std::string> arguments;
			unsigned int p_args = split_str(arg, arguments);
			
			//We clear the error flag when a command is entered.
			had_error = false;
			// check for defined commands
			if(cmd == "quit" || cmd == "exit"){
				if(do_MCA_run){ std::cout << sys_message_head << "Warning! Cannot quit while MCA program is running\n"; }
				else if(acq_running){ std::cout << sys_message_head << "Warning! Cannot quit while acquisition running\n"; }
				else{
					kill_all = true;
					while(!run_ctrl_exit){ sleep(1); }
					break;
				}
			}
			else if(cmd == "kill"){
				if(acq_running || do_MCA_run){ 
					std::cout << sys_message_head << "Sending KILL signal\n";
					stop_acq = true; 
				}
				kill_all = true;
				while(!run_ctrl_exit){ sleep(1); }
				break;
			}
			else if(cmd == "help" || cmd == "h"){ help(); }
			else if(cmd == "version" || cmd == "v"){ 
				std::cout << "  Poll2 Core    v" << POLL2_CORE_VERSION << " (" << POLL2_CORE_DATE << ")\n"; 
				std::cout << "  Poll2 Socket  v" << POLL2_SOCKET_VERSION << " (" << POLL2_SOCKET_DATE << ")\n"; 
				std::cout << "  HRIBF Buffers v" << HRIBF_BUFFERS_VERSION << " (" << HRIBF_BUFFERS_DATE << ")\n"; 
				std::cout << "  CTerminal     v" << CTERMINAL_VERSION << " (" << CTERMINAL_DATE << ")\n";
			}
			else if(cmd == "status"){
				std::cout << "  Poll Run Status:\n";
				std::cout << "   Acq starting    - " << yesno(start_acq) << std::endl;
				std::cout << "   Acq stopping    - " << yesno(stop_acq) << std::endl;
				std::cout << "   Acq running     - " << yesno(acq_running) << std::endl;
				std::cout << "   Shared memory   - " << yesno(shm_mode) << std::endl;
				std::cout << "   Write to disk   - " << yesno(record_data) << std::endl;
				std::cout << "   File open       - " << yesno(output_file.IsOpen()) << std::endl;
				std::cout << "   Rebooting       - " << yesno(do_reboot) << std::endl;
				std::cout << "   Force Spill     - " << yesno(force_spill) << std::endl;
				std::cout << "   Run ctrl Exited - " << yesno(run_ctrl_exit) << std::endl;
				std::cout << "   Do MCA run      - " << yesno(do_MCA_run) << std::endl;

				std::cout << "\n  Poll Options:\n";
				std::cout << "   Boot fast   - " << yesno(boot_fast) << std::endl;
				std::cout << "   Wall clock  - " << yesno(insert_wall_clock) << std::endl;
				std::cout << "   Is quiet    - " << yesno(is_quiet) << std::endl;
				std::cout << "   Send alarm  - " << yesno(send_alarm) << std::endl;
				std::cout << "   Show rates  - " << yesno(show_module_rates) << std::endl;
				std::cout << "   Zero clocks - " << yesno(zero_clocks) << std::endl;
				std::cout << "   Debug mode  - " << yesno(debug_mode) << std::endl;
				std::cout << "   Initialized - " << yesno(init) << std::endl;
			}
			// Tell POLL to start acq and start recording data to disk
			else if(cmd == "run"){ StartRun(); } 
			else if(cmd == "startacq" || cmd == "startvme"){ // Tell POLL to start data acquisition
				StartAcq();
			}
			else if(cmd == "stop"){ // Tell POLL to stop recording data to disk and stop acq
				StopRun();
			} 
			else if(cmd == "stopacq" || cmd == "stopvme"){ // Tell POLL to stop data acquisition
				StopAcq();
			}
			else if(cmd == "acq" || cmd == "shm"){ // Toggle "shared-memory" mode
				if(shm_mode){
					std::cout << sys_message_head << "Toggling shared-memory mode OFF\n";
					shm_mode = false;
				}
				else{
					std::cout << sys_message_head << "Toggling shared-memory mode ON\n";
					shm_mode = true;
				}
			}
			else if(cmd == "reboot"){ // Tell POLL to attempt a PIXIE crate reboot
				if(do_MCA_run){ std::cout << sys_message_head << "Warning! Cannot reboot while MCA is running\n"; }
				else if(acq_running || do_MCA_run){ std::cout << sys_message_head << "Warning! Cannot reboot while acquisition running\n"; }
				else{ 
					do_reboot = true; 
					poll_term_->pause(do_reboot);
				}
			}
			else if(cmd == "clo" || cmd == "close"){ // Tell POLL to close the current data file
				if(do_MCA_run){ std::cout << sys_message_head << "Command not available for MCA run\n"; }
				else if(acq_running && record_data){ std::cout << sys_message_head << "Warning! Cannot close file while acquisition running\n"; }
				else{ close_output_file(); }
			}
			else if(cmd == "hup" || cmd == "spill"){ // Force spill
				if(do_MCA_run){ std::cout << sys_message_head << "Command not available for MCA run\n"; }
				else if(!acq_running){ std::cout << sys_message_head << "Acquisition is not running\n"; }
				else{ force_spill = true; }
			}
			else if(cmd == "debug"){ // Toggle debug mode
				if(debug_mode){
					std::cout << sys_message_head << "Toggling debug mode OFF\n";
					output_file.SetDebugMode(false);
					debug_mode = false;
				}
				else{
					std::cout << sys_message_head << "Toggling debug mode ON\n";
					output_file.SetDebugMode();
					debug_mode = true;
				}
			}
			else if(cmd == "quiet"){ // Toggle quiet mode
				if(is_quiet){
					std::cout << sys_message_head << "Toggling quiet mode OFF\n";
					is_quiet = false;
				}
				else{
					std::cout << sys_message_head << "Toggling quiet mode ON\n";
					is_quiet = true;
				}
			}
			else if(cmd == "fdir"){ // Change the output file directory
				if (arg == "") { std::cout << sys_message_head << "Using output directory '" << output_directory << "'\n"; }
				else if (file_open) {
					std::cout << sys_message_head << Display::WarningStr("Warning:") << " Directory cannot be changed while a file is open!\n";
				}
				else {
					output_directory = arg; 
					current_file_num = 0;
				
					// Append a '/' if the user did not include one
					if(*(output_directory.end()-1) != '/'){ output_directory += '/'; }

					std::cout << sys_message_head << "Set output directory to '" << output_directory << "'.\n";

					//Check what run files already exist.
					int temp_run_num = next_run_num;
					std::string filename = output_file.GetNextFileName(next_run_num,filename_prefix, output_directory);
					if (temp_run_num != next_run_num) {
						std::cout << sys_message_head << Display::WarningStr("Warning") << ": Run file existed for run " << temp_run_num << "! Next run number will be " << next_run_num << ".\n";
					}

					std::cout << sys_message_head << "Next file will be '" << filename << "'.\n";
				}
			} 
			else if (cmd == "prefix") {
				if (arg == "") {
					std::cout << sys_message_head << "Using output filename prefix '" << filename_prefix << "'.\n";
				}
				else if (file_open) {
					std::cout << sys_message_head << Display::WarningStr("Warning:") << " Prefix cannot be changed while a file is open!\n";
				}
				else {
					filename_prefix = arg;
					next_run_num = 1;

					//Check what run files already exist.
					std::string filename = output_file.GetNextFileName(next_run_num,filename_prefix, output_directory);
					if (next_run_num != 1) {
						std::cout << sys_message_head << Display::WarningStr("Warning") << ": Some run files existed! Next run number will be " << next_run_num << ".\n";
					}

					std::cout << sys_message_head << "Set output filename prefix to '" << filename_prefix << "'.\n";
					std::cout << sys_message_head << "Next file will be '" << output_file.GetNextFileName(next_run_num,filename_prefix, output_directory) << "'.\n";
				}
			}
			else if(cmd == "title"){ // Change the title of the output file
				if (arg == "") { std::cout << sys_message_head << "Using output file title '" << output_title << "'.\n"; }
				else if (file_open) {
					std::cout << sys_message_head << Display::WarningStr("Warning:") << " Run title cannot be changed while a file is open!\n";
				}
				else {
					output_title = arg; 
					std::cout << sys_message_head << "Set run title to '" << output_title << "'.\n";
				}
			}
			else if(cmd == "facility"){ // Change the facility of the output file
				if(arg == ""){ 
					if(output_format != 1){ std::cout << sys_message_head << "Using output file facility '" << output_file.GetHEADbuffer()->GetFacility() << "'.\n"; }
					else{ std::cout << sys_message_head << "Using output file facility '" << output_file.GetPLDheader()->GetFacility() << "'.\n"; }
				}
				else if(output_format != 1){ std::cout << sys_message_head << "Facility may only be changed for pld output format!\n"; }
				else if (file_open){ std::cout << sys_message_head << Display::WarningStr("Warning:") << " Run facility cannot be changed while a file is open!\n"; }
				else{
					output_file.GetPLDheader()->SetFacility(arg);
					std::cout << sys_message_head << "Set run facility to '" << output_file.GetPLDheader()->GetFacility() << "'.\n";
				}
			}
			else if(cmd == "runnum"){ // Change the run number to the specified value
				if (arg == "") { 
					if (output_file.IsOpen()) 
						std::cout << sys_message_head << "Current output file run number '" << output_file.GetRunNumber() << "'.\n"; 
					if (!output_file.IsOpen() || next_run_num != output_file.GetRunNumber()) 
						std::cout << sys_message_head << "Next output file run number '" << next_run_num << "' for prefix '" << filename_prefix << "'.\n"; 
				}
				else if (file_open) {
					std::cout << sys_message_head << Display::WarningStr("Warning:") << " Run number cannot be changed while a file is open!\n";
				}
				else {
					next_run_num = atoi(arg.c_str()); 
					std::string filename = output_file.GetNextFileName(next_run_num,filename_prefix, output_directory);
					if (next_run_num != atoi(arg.c_str())) {
						std::cout << sys_message_head << Display::WarningStr("Wanring") << ": Run file existed for run " << atoi(arg.c_str()) << ".\n";
					}
					std::cout << sys_message_head << "Set run number to '" << next_run_num << "'.\n";
					std::cout << sys_message_head << "Next file will be '" << filename << "'.\n";
				}
			} 
			else if(cmd == "oform"){ // Change the output file format
				if(arg != ""){
					int format = atoi(arg.c_str());
					if(format == 0 || format == 1 || format == 2){
						output_format = atoi(arg.c_str());
						std::cout << sys_message_head << "Set output file format to '" << output_format << "'\n";
						if(output_format == 1){ std::cout << "  Warning! This output format is experimental and is not recommended for data taking\n"; }
						else if(output_format == 2){ std::cout << "  Warning! This output format is experimental and is not recommended for data taking\n"; }
						output_file.SetFileFormat(output_format);
					}
					else{ 
						std::cout << sys_message_head << "Unknown output file format ID '" << format << "'\n";
						std::cout << "  Available file formats include:\n";
						std::cout << "   0 - .ldf (HRIBF) file format (default)\n";
						std::cout << "   1 - .pld (PIXIE) file format (experimental)\n";
						std::cout << "   2 - .root file format (slow, not recommended)\n";
					}
				}
				else{ std::cout << sys_message_head << "Using output file format '" << output_format << "'\n"; }
				if(output_file.IsOpen()){ std::cout << sys_message_head << "New output format used for new files only! Current file is unchanged.\n"; }
			}
			else if(cmd == "mca" || cmd == "MCA"){ // Run MCA program using either root or damm
				if(do_MCA_run){
					std::cout << sys_message_head << "MCA program is already running\n\n";
					continue;
				}
				else if(acq_running){ 
					std::cout << sys_message_head << "Warning! Cannot run MCA program while acquisition is running\n\n";
					continue;
				}

				if (p_args >= 1) {
					std::string type = arguments.at(0);
					if(type == "root"){ mca_args.useRoot = true; }
					else if(type != "damm"){ mca_args.totalTime = atoi(type.c_str()); }
					if(p_args >= 2){
						if(mca_args.totalTime == 0){ mca_args.totalTime = atoi(arguments.at(1).c_str()); }
						else{ mca_args.basename = arguments.at(1); }
						if(p_args >= 3){ mca_args.basename = arguments.at(2); }
					}
				}
				if(mca_args.totalTime == 0){ 
					mca_args.totalTime = 10; 
					std::cout << sys_message_head << "Using default MCA time of 10 seconds\n";
				}
			
				do_MCA_run = true;
			}
			else if(cmd == "dump"){ // Dump pixie parameters to file
				std::ofstream ofile;
				
				if(p_args >= 1){
					ofile.open(arg.c_str());
					if(!ofile.good()){
						std::cout << sys_message_head << "Failed to open output file '" << arg << "'\n";
						std::cout << sys_message_head << "Check that the path is correct\n";
						continue;
					}
				}
				else{
					ofile.open("./Fallback.set");
					if(!ofile.good()){
						std::cout << sys_message_head << "Failed to open output file './Fallback.set'\n";
						continue;
					}
				}

				ParameterChannelDumper chanReader(&ofile);
				ParameterModuleDumper modReader(&ofile);

				// Channel dependent settings
				for(unsigned int param = 0; param < chan_params.size(); param++){
					forChannel<std::string>(pif, -1, -1, chanReader, chan_params[param]);
				}

				// Channel independent settings
				for(unsigned int param = 0; param < mod_params.size(); param++){
					forModule(pif, -1, modReader, mod_params[param]);
				}

				if(p_args >= 1){ std::cout << sys_message_head << "Successfully wrote output parameter file '" << arg << "'\n"; }
				else{ std::cout << sys_message_head << "Successfully wrote output parameter file './Fallback.set'\n"; }
				ofile.close();
			}
			else if(cmd == "pwrite" || cmd == "pmwrite"){ // Write pixie parameters
				if(acq_running || do_MCA_run){ 
					std::cout << sys_message_head << "Warning! Cannot edit pixie parameters while acquisition is running\n\n"; 
					continue;
				}
			
				if(cmd == "pwrite"){ // Syntax "pwrite <module> <channel> <parameter name> <value>"
					if(p_args > 0 && arguments.at(0) == "help"){ pchan_help(); }
					else if(p_args >= 4){
						int mod = atoi(arguments.at(0).c_str());
						int ch = atoi(arguments.at(1).c_str());
						double value = std::strtod(arguments.at(3).c_str(), NULL);
					
						ParameterChannelWriter writer;
						if(forChannel(pif, mod, ch, writer, make_pair(arguments.at(2), value))){ pif->SaveDSPParameters(); }
					}
					else{
						std::cout << sys_message_head << "Invalid number of parameters to pwrite\n";
						std::cout << sys_message_head << " -SYNTAX- pwrite [module] [channel] [parameter] [value]\n";
					}
				}
				else if(cmd == "pmwrite"){ // Syntax "pmwrite <module> <parameter name> <value>"
					if(p_args > 0 && arguments.at(0) == "help"){ pmod_help(); }
					else if(p_args >= 3){
						int mod = atoi(arguments.at(0).c_str());
						unsigned int value = (unsigned int)std::strtoul(arguments.at(2).c_str(), NULL, 0);
					
						ParameterModuleWriter writer;
						if(forModule(pif, mod, writer, make_pair(arguments.at(1), value))){ pif->SaveDSPParameters(); }
					}
					else{
						std::cout << sys_message_head << "Invalid number of parameters to pmwrite\n";
						std::cout << sys_message_head << " -SYNTAX- pmwrite [module] [parameter] [value]\n";
					}
				}
			}
			else if(cmd == "pread" || cmd == "pmread"){ // Read pixie parameters
				if(cmd == "pread"){ // Syntax "pread <module> <channel> <parameter name>"
					if(p_args > 0 && arguments.at(0) == "help"){ pchan_help(); }
					else if(p_args >= 3){
						int mod = atoi(arguments.at(0).c_str());
						int ch = atoi(arguments.at(1).c_str());
					
						ParameterChannelReader reader;
						forChannel(pif, mod, ch, reader, arguments.at(2));
					}
					else{
						std::cout << sys_message_head << "Invalid number of parameters to pread\n";
						std::cout << sys_message_head << " -SYNTAX- pread [module] [channel] [parameter]\n";
					}
				}
				else if(cmd == "pmread"){ // Syntax "pmread <module> <parameter name>"
					if(p_args > 0 && arguments.at(0) == "help"){ pmod_help(); }
					else if(p_args >= 2){
						int mod = atoi(arguments.at(0).c_str());
					
						ParameterModuleReader reader;
						forModule(pif, mod, reader, arguments.at(1));
					}
					else{
						std::cout << sys_message_head << "Invalid number of parameters to pmread\n";
						std::cout << sys_message_head << " -SYNTAX- pread [module] [parameter]\n";
					}
				}
			}
			else if(cmd == "adjust_offsets"){ // Run adjust_offsets
				if(acq_running || do_MCA_run){ 
					std::cout << sys_message_head << "Warning! Cannot edit pixie parameters while acquisition is running\n\n"; 
					continue;
				}

				if(p_args >= 1){
					int mod = atoi(arguments.at(0).c_str());
					
					OffsetAdjuster adjuster;
					if(forModule(pif, mod, adjuster, 0)){ pif->SaveDSPParameters(); }
				}
				else{
					std::cout << sys_message_head << "Invalid number of parameters to adjust_offsets\n";
					std::cout << sys_message_head << " -SYNTAX- adjust_offsets [module]\n";
				}
			}
			else if(cmd == "find_tau"){ // Run find_tau
				if(acq_running || do_MCA_run){ 
					std::cout << sys_message_head << "Warning! Cannot edit pixie parameters while acquisition is running\n\n"; 
					continue;
				}
			
				if(p_args >= 2){
					int mod = atoi(arguments.at(0).c_str());
					int ch = atoi(arguments.at(1).c_str());

					TauFinder finder;
					forChannel(pif, mod, ch, finder, 0);
				}
				else{
					std::cout << sys_message_head << "Invalid number of parameters to find_tau\n";
					std::cout << sys_message_head << " -SYNTAX- find_tau [module] [channel]\n";
				}
			}
			else if(cmd == "toggle"){ // Toggle a CHANNEL_CSRA bit
				if(acq_running || do_MCA_run){ 
					std::cout << sys_message_head << "Warning! Cannot edit pixie parameters while acquisition is running\n\n"; 
					continue;
				}

				BitFlipper flipper;

				if(p_args >= 3){ 
					flipper.SetCSRAbit(arguments.at(2));
					
					std::string dum_str = "CHANNEL_CSRA";
					if(forChannel(pif, atoi(arguments.at(0).c_str()), atoi(arguments.at(1).c_str()), flipper, dum_str)){
						pif->SaveDSPParameters();
					}
				}
				else{
					std::cout << sys_message_head << "Invalid number of parameters to toggle\n";
					std::cout << sys_message_head << " -SYNTAX- toggle [module] [channel] [CSRA bit]\n\n";
					flipper.Help();				
				}
			}
			else if(cmd == "toggle_bit"){ // Toggle any bit of any parameter under 32 bits long
				if(acq_running || do_MCA_run){ 
					std::cout << sys_message_head << "Warning! Cannot edit pixie parameters while acquisition is running\n\n"; 
					continue;
				}

				BitFlipper flipper;

				if(p_args >= 4){ 
					flipper.SetBit(arguments.at(3));
    
					if(forChannel(pif, atoi(arguments.at(0).c_str()), atoi(arguments.at(1).c_str()), flipper, arguments.at(2))){
						pif->SaveDSPParameters();
					}
				}
				else{
					std::cout << sys_message_head << "Invalid number of parameters to toggle_any\n";
					std::cout << sys_message_head << " -SYNTAX- toggle_any [module] [channel] [parameter] [bit]\n\n";
				}
			}
			else if(cmd == "csr_test"){ // Run CSRAtest method
				BitFlipper flipper;
				if(p_args >= 1){ flipper.CSRAtest((unsigned int)atoi(arguments.at(0).c_str())); }
				else{
					std::cout << sys_message_head << "Invalid number of parameters to csr_test\n";
					std::cout << sys_message_head << " -SYNTAX- csr_test [number]\n";
				}
			}
			else if(cmd == "bit_test"){ // Run Test method
				BitFlipper flipper;
				if(p_args >= 2){ flipper.Test((unsigned int)atoi(arguments.at(0).c_str()), std::strtoul(arguments.at(1).c_str(), NULL, 0)); }
				else{
					std::cout << sys_message_head << "Invalid number of parameters to bit_test\n";
					std::cout << sys_message_head << " -SYNTAX- bit_test [num_bits] [number]\n";
				}
			}
			else{ std::cout << sys_message_head << "Unknown command '" << cmd << "'\n"; }

		}
	}
}

///////////////////////////////////////////////////////////////////////////////
// Poll::run_control
///////////////////////////////////////////////////////////////////////////////

/// Function to control the gathering and recording of PIXIE data
void Poll::run_control(){
	while(true){
		if(kill_all){ // Supersedes all other commands
			if(acq_running){ stop_acq = true; } // Safety catch
			else{ break; }
		}
		
		if(do_reboot){ // Attempt to reboot the PIXIE crate
			if(acq_running){ stop_acq = true; } // Safety catch
			else{
				std::cout << sys_message_head << "Attempting PIXIE crate reboot\n";
				pif->Boot(PixieInterface::BootAll);
				printf("Press any key to continue...");
				std::cin.get();
				do_reboot = false;
			}
		}

		if(do_MCA_run){ // Do an MCA run, if the acq is not running
			if(acq_running){ stop_acq = true; } // Safety catch
			else{
				if(mca_args.totalTime > 0.0){ std::cout << sys_message_head << "Performing MCA data run for " << mca_args.totalTime << " s\n"; }
				else{ std::cout << sys_message_head << "Performing infinite MCA data run. Type \"stop\" to quit\n"; }
				pif->RemovePresetRunLength(0);

				MCA *mca = NULL;
#if defined(USE_ROOT) && defined(USE_DAMM)
				if(mca_args.useRoot){ mca = new MCA_ROOT(pif, mca_args.basename.c_str()); }
				else{ mca = new MCA_DAMM(pif, mca_args.basename.c_str()); }
#elif defined(USE_ROOT)
				mca = new MCA_ROOT(pif, mca_args.basename.c_str());
#elif defined(USE_DAMM)
				mca = new MCA_DAMM(pif, mca_args.basename.c_str());
#endif

				if(mca && mca->IsOpen()){ mca->Run(mca_args.totalTime, &stop_acq); }
				mca_args.Zero();
				stop_acq = false;
				do_MCA_run = false;
				delete mca;
				
				std::cout << std::endl;
			}
		}

		//Start acquistion
		if (start_acq && !acq_running) {
			//Start list mode
			if(pif->StartListModeRun(LIST_MODE_RUN, NEW_RUN)) {
				time_t currTime;
				time(&currTime);
				if (record_data) std::cout << "Run " << output_file.GetRunNumber();
				else std::cout << "Acq";
				std::cout << " started on " << ctime(&currTime);

				acq_running = true;
				startTime = usGetTime(0);
				lastSpillTime = 0;
			}
			else{ 
				std::cout << sys_message_head << "Failed to start list mode run. Try rebooting PIXIE\n"; 
				acq_running = false;
				had_error = true;
			}
			start_acq = false;
		}
		else if (start_acq && acq_running) {
			std::cout << sys_message_head << "Already running!\n";
			start_acq = false;
		}

		if(acq_running){
			ReadFIFO();

			//Handle a stop signal
			if(stop_acq){ 
				pif->EndRun();

				time_t currTime;
				time(&currTime);
				
				//Reset status flags
				stop_acq = false;
				acq_running = false;

				// Check if each module has ended its run properly.
				for(size_t mod = 0; mod < n_cards; mod++){
					//If the run status is 1 then the run has not finished in the module.
					// We need to read it out.
					if(pif->CheckRunStatus(mod) == 1) {
						if (!is_quiet) std::cout << "Module " << mod << " still has " << pif->CheckFIFOWords(mod) << " words in the FIFO.\n";
						//We set force_spill to true in case the remaining words is small.
						force_spill = true;
						//We sleep to allow the module to finish.
						sleep(1);
						//We read the FIFO out.
						ReadFIFO();
					}

					//Print the module status.
					std::stringstream leader;
					leader << "Run end status in module " << mod;
					Display::LeaderPrint(leader.str());
					if(!pif->CheckRunStatus(mod)){
						std::cout << Display::OkayStr() << std::endl;
					}
					else {
						std::cout << Display::ErrorStr() << std::endl;
						had_error = true;
					}
				}

				if (record_data) std::cout << "Run " << output_file.GetRunNumber();
				else std::cout << "Acq";
				std::cout << " stopped on " << ctime(&currTime);
			} //End of handling a stop acq flag
		}

		//Build status string
		std::stringstream status;
		if (had_error) status << Display::ErrorStr("[ERROR]");
		else if (acq_running && record_data) status << Display::OkayStr("[ACQ]");
		else if (acq_running && !record_data) status << Display::WarningStr("[ACQ]");
		else if (do_MCA_run) status << Display::OkayStr("[MCA]");
		else status << Display::InfoStr("[IDLE]");

		if (file_open) status << " Run " << output_file.GetRunNumber();

		//Add run time to status
		status << " " << (long long) statsHandler->GetTotalTime() << "s";
		//Add data rate to status
		status << " " << humanReadable(statsHandler->GetTotalDataRate()) << "/s";

		if (file_open) {
			if (acq_running && !record_data) status << TermColors::DkYellow;
			//Add file size to status
			status << " " << humanReadable(output_file.GetFilesize());
			status << " " << output_file.GetCurrentFilename();
			if (acq_running && !record_data) status << TermColors::Reset;
		}

		//Update the status bar
		poll_term_->SetStatus(status.str());

		//Sleep the run control if idle to reduce CPU utilization.
		if (!acq_running && !do_MCA_run) sleep(1);
	}

	run_ctrl_exit = true;
	std::cout << "Run Control exited\n";
}

bool Poll::ReadFIFO() {
 	static word_t *fifoData = new word_t[(EXTERNAL_FIFO_LENGTH + 2) * n_cards];

	if (!acq_running) return false;

	//Number of words in the FIFO of each module.
	std::vector<word_t> nWords(n_cards);
	//Iterator to determine which card has the most words.
	std::vector<word_t>::iterator maxWords;
	//A vector to store the partial events
	static std::vector<word_t> *partialEvent = new std::vector<word_t>[n_cards];

	//We loop until the FIFO has reached the threshold for any module
	for (unsigned int timeout = 0; timeout < POLL_TRIES; timeout++){ 
		//Check the FIFO size for every module
		for (unsigned short mod=0; mod < n_cards; mod++) {
			nWords[mod] = pif->CheckFIFOWords(mod);
		}
		//Find the maximum module
		maxWords = std::max_element(nWords.begin(), nWords.end());
		if(*maxWords > threshWords){ break; }
	}
	//Decide if we should read data based on threshold.
	bool readData = (*maxWords > threshWords || stop_acq);

	//We need to read the data out of the FIFO
	if (readData || force_spill) {
		force_spill = false;
		//Number of data words read from the FIFO
		size_t dataWords = 0;

		//Loop over each module's FIFO
		for (unsigned short mod=0;mod < n_cards; mod++) {

			//if the module has no words in the FIFO we continue to the next module
			if (nWords[mod] < MIN_FIFO_READ) {
				// write an empty buffer if there is no data
				fifoData[dataWords++] = 2;
				fifoData[dataWords++] = mod;	    
				continue;
			}
			else if (nWords[mod] < 0) {
				std::cout << Display::WarningStr("Number of FIFO words less than 0") << " in module " << mod << std::endl;
				// write an empty buffer if there is no data
				fifoData[dataWords++] = 2;
				fifoData[dataWords++] = mod;	    
				continue;
			}

			//Check if the FIFO is overfilled
			bool fullFIFO = (nWords[mod] >= EXTERNAL_FIFO_LENGTH);
			if (fullFIFO) {
				std::cout << Display::ErrorStr() << " Full FIFO in module " << mod 
					<< " size: " << nWords[mod] << "/" 
					<< EXTERNAL_FIFO_LENGTH << Display::ErrorStr(" ABORTING!") << std::endl;
				had_error = true;
				stop_acq = true;
				return false;
			}

			//We inject two words describing the size of the FIFO spill and the module.
			//We inject the size after it has been computedos we skip it for now and only add the module number.
			dataWords++;
			fifoData[dataWords++] = mod;

			//We store the partial event if we had one
			for (size_t i=0;i<partialEvent[mod].size();i++)
				fifoData[dataWords + i] = partialEvent[mod].at(i);

			//Try to read FIFO and catch errors.
			if(!pif->ReadFIFOWords(&fifoData[dataWords + partialEvent[mod].size()], nWords[mod], mod, debug_mode)){
				std::cout << Display::ErrorStr() << " Unable to read " << nWords[mod] << " from module " << mod << "\n";
				had_error = true;
				stop_acq = true;
				return false;
			}

			//Print a message about what we did	
			if(!is_quiet) {
				std::cout << "Read " << nWords[mod] << " words from module " << mod;
				if (!partialEvent[mod].empty())
					std::cout << " and stored " << partialEvent[mod].size() << " partial event words";
				std::cout << " to buffer position " << dataWords << std::endl;
			}

			//After reading the FIFO and printing a sttus message we can update the number of words to include the partial event.
			nWords[mod] += partialEvent[mod].size();
			//Clear the partial event
			partialEvent[mod].clear();

			//We now ned to parse the event to determine if there is a hanging event. Also, allows a check for corrupted data.
			size_t parseWords = dataWords;
			//We declare the eventSize outside the loop in case there is a partial event.
			word_t eventSize = 0;
			while (parseWords < dataWords + nWords[mod]) {
				//Check first word to see if data makes sense.
				// We check the slot, channel and event size.
				word_t slotRead = ((fifoData[parseWords] & 0xF0) >> 4);
				word_t slotExpected = pif->GetSlotNumber(mod);
				word_t chanRead = (fifoData[parseWords] & 0xF);
				eventSize = ((fifoData[parseWords] & 0x7FFE2000) >> 17);
				bool virtualChannel = ((fifoData[parseWords] & 0x20000000) != 0);

				if( slotRead != slotExpected ){ 
					std::cout << Display::ErrorStr() << " Slot read (" << slotRead 
						<< ") not the same as" << " slot expected (" 
						<< slotExpected << ")" << std::endl; 
					break;
				}
				else if (chanRead < 0 || chanRead > 15) {
					std::cout << Display::ErrorStr() << " Channel read (" << chanRead << ") not valid!\n";
					break;
				}
				else if(eventSize == 0){ 
					std::cout << Display::ErrorStr() << "ZERO EVENT SIZE in mod " << mod << "!\n"; 
					break;
				}

				// Update the statsHandler with the event (for monitor.bash)
				if(!virtualChannel && statsHandler){ 
					statsHandler->AddEvent(mod, chanRead, sizeof(word_t) * eventSize); 
				}

				//Iterate to the next event and continue parsing
				parseWords += eventSize;
			}

			//We now check the outcome of the data parsing.
			//If we have too many words as an event was not completely pulled form the FIFO
			if (parseWords > dataWords + nWords[mod]) {
				word_t missingWords = parseWords - dataWords - nWords[mod];
				word_t partialSize = eventSize - missingWords;
				if (debug_mode) std::cout << "Partial event " << partialSize << "/" << eventSize << " words!\n";

				//We could get the words now from the FIFO, but me may have to wait. Instead we store the partial event for the next FIFO read.
				for(unsigned short i=0;i< partialSize;i++) 
					partialEvent[mod].push_back(fifoData[parseWords - eventSize + i]);

				//Update the number of words to indicate removal or partial event.
				nWords[mod] -= partialSize;

			}
			//If parseWords is small then the parse failed for some reason
			else if (parseWords < dataWords + nWords[mod]) {
				std::cout << Display::ErrorStr() << " Parsing indicated corrupted data at " << parseWords - dataWords << " words into FIFO.\n";

				if (!is_quiet) {
					//Print the first 100 words
					std::cout << std::hex;
					for(int i=0;i< 100;i++) {
						if (i%10 == 0) std::cout << std::endl << "\t";
						std::cout << fifoData[dataWords + i] << " ";
					}
					std::cout << std::dec << std::endl;
				}

				stop_acq = true;
				had_error = true;
				return false;
			}

			//Assign the first injected word of spill to final spill length
			fifoData[dataWords - 2] = nWords[mod] + 2;
			//The data should be good so we iterate the position in the storage array.
			dataWords += nWords[mod];
		} //End loop over modules for reading FIFO

		if (!is_quiet) std::cout << "Writing/Broadcasting " << dataWords << " words.\n";
		//We have read the FIFO now we write the data	
		if (record_data) write_data(fifoData, dataWords); 
		broadcast_data(fifoData, dataWords);

		//Get the length of the spill
		double spillTime = usGetTime(startTime);
		double durSpill = spillTime - lastSpillTime;
		lastSpillTime = spillTime;
		// Add time to the statsHandler (for monitor.bash)
		if(statsHandler){ statsHandler->AddTime(durSpill * 1e-6); }
	} //If we had exceeded the threshold or forced a flush

	return true;
}

///////////////////////////////////////////////////////////////////////////////
// Support Functions
///////////////////////////////////////////////////////////////////////////////

std::string humanReadable(double size) {
	int power = std::log10(size);
	std::stringstream output;
	output << std::setprecision(3);
	if (power >= 9) output << size/pow(1024,3) << "GB";
	else if (power >= 6) output << size/pow(1024,2) << "MB";
	else if (power >= 3) output << size/1024 << "kB";
	else output << " " << size << "B";
	return output.str(); 
}


unsigned int split_str(std::string str_, std::vector<std::string> &args, char delimiter_){
	args.clear();
	std::string temp = "";
	unsigned int count = 0;
	for(unsigned int i = 0; i < str_.size(); i++){
		if(str_[i] == delimiter_ || i == str_.size()-1){
			if(i == str_.size()-1){ temp += str_[i]; }
			args.push_back(temp);
			temp = "";
			count++;
		}
		else{ temp += str_[i]; }		
	}
	return count;
}

/* Pad a string with '.' to a specified length. */
std::string pad_string(const std::string &input_, unsigned int length_){
	std::string output = input_;
	for(unsigned int i = input_.size(); i <= length_; i++){
		output += '.';
	}
	return output;
}

std::string yesno(bool value_){
	if(value_){ return "Yes"; }
	return "No";
}
