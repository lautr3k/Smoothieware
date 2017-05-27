/*
This file is part of Smoothie (http://smoothieware.org/). The motion control part is heavily based on Grbl (https://github.com/simen/grbl).
Smoothie is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
Smoothie is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
You should have received a copy of the GNU General Public License along with Smoothie. If not, see <http://www.gnu.org/licenses/>.
*/

#include "Player.h"

#include "libs/Kernel.h"
#include "Gcode.h"
#include "libs/utils.h"
#include "libs/StreamOutput.h"
#include "libs/SerialMessage.h"
#include "libs/StreamOutputPool.h"

#include "SDFAT.h"
extern SDFAT mounter;

Player::Player()
{
    this->output_stream = NULL;
    this->current_file  = { "", NULL, 0, 0 }; // { path, file, size, read }
    this->playing       = false;
}

void Player::on_module_loaded()
{
    this->register_for_event(ON_CONSOLE_LINE_RECEIVED);
    this->register_for_event(ON_GCODE_RECEIVED);
    this->register_for_event(ON_MAIN_LOOP);
}

// ON_MAIN_LOOP ----------------------------------------------------------------

void Player::on_main_loop(void* argument)
{
    // exit, if in halted state
    if (THEKERNEL->is_halted()) {
        return;
    }

    // exit, if no in play mode
    if (!this->playing) {
        return;
    }

    char buf[130]; // lines upto 128 characters are allowed, anything longer is discarded
    bool discard = false;

    while(fgets(buf, sizeof(buf), this->current_file.file) != NULL) {
        int len = strlen(buf);

        // skip empty line (not possible ?)
        if (len == 0) {
            continue;
        }

        // save readed bytes
        this->current_file.read += len;

        if (buf[len - 1] == '\n' || feof(this->current_file.file)) {
            // we are discarding a long line
            if (discard) {
                discard = false;
                continue;
            }

            // skip empty line
            if (len == 1) {
                continue;
            }

            // create serial message
            struct SerialMessage serial;
            serial.message = buf;
            serial.stream  = this->output_stream;

            // waits for the queue to have enough room
            THEKERNEL->call_event(ON_CONSOLE_LINE_RECEIVED, &serial);

            return; // we feed one line per main loop
        }
        else {
            // discard long line
            this->output_stream->printf("Warning: Discarded long line\n");
            discard = true;
        }
    }

    this->output_stream->printf("Print done\n");
    this->pause_file();
}

// FILE HELPERS ----------------------------------------------------------------

bool Player::open_file(string path)
{
    // pause playing file
    this->pause_file();

    // close opened file
    this->close_file();

    // set new file { path, file, size, read }
    this->current_file = { path, NULL, 0, 0 };

    // DEBUG
    THEKERNEL->streams->printf("open_file: %s\r\n", this->current_file.path.c_str());

    // open new file
    FILE* file = fopen(this->current_file.path.c_str(), "r");

    // DEBUG
    THEKERNEL->streams->printf("openend: %s\r\n", this->current_file.path.c_str());

    // set file handler and size
    if (0 != file) {
        this->current_file.file = file;
        this->get_filesize();

        // DEBUG
        THEKERNEL->streams->printf("file size: %ld\r\n", this->current_file.size);

        return true;
    }

    return false;
}

void Player::get_filesize()
{
    int result = fseek(this->current_file.file, 0, SEEK_END);

    if (0 != result) {
        this->current_file.size = 0;
    }
    else {
        this->current_file.size = ftell(this->current_file.file);
        fseek(this->current_file.file, 0, SEEK_SET);
    }
}

void Player::play_file()
{
    // DEBUG
    //THEKERNEL->streams->printf("play_file: %s\r\n", this->current_file.path.c_str());

    // start play if file is opened
    this->playing = this->current_file.file != NULL;
}

void Player::pause_file()
{
    // DEBUG
    //THEKERNEL->streams->printf("pause_file: %s\r\n", this->current_file.path.c_str());

    // pause play
    this->playing = false;
}

void Player::close_file()
{
    if (this->current_file.file != NULL) {
        fclose(this->current_file.file);
        this->current_file.file = NULL;
    }
}

// ON_CONSOLE_LINE_RECEIVED ----------------------------------------------------

void Player::on_console_line_received(void* argument)
{
    // exit, if in halted state
    if (THEKERNEL->is_halted()) {
        return;
    }

    // get command line from (serial) message
    SerialMessage serial = *static_cast<SerialMessage *>(argument);
    string arguments     = serial.message;

    // ignore anything that is not lowercase or a letter
    if (arguments.empty() || !islower(arguments[0]) || !isalpha(arguments[0])) {
        return;
    }

    // set output stream
    //if (this->output_stream == NULL) {
        this->output_stream = serial.stream;
    //}

    // DEBUG
    THEKERNEL->streams->printf("ON_CONSOLE_LINE_RECEIVED: %s\r\n", arguments.c_str());

    // extract command name
    string cmd = shift_parameter(arguments);

    // Act depending on command
    if (cmd == "play") {
        this->play_command(arguments);
    } else if (cmd == "progress") {
        this->progress_command(arguments);
    } else if (cmd == "abort") {
        this->abort_command(arguments);
    } else if (cmd == "suspend") {
        this->suspend_command(arguments);
    } else if (cmd == "resume") {
        this->resume_command(arguments);
    }
}

// extract any options found on line, terminates arguments at the space before the first option (-v)
// eg this is a file.gcode -v
//    will return -v and set arguments to this is a file.gcode
string Player::extract_options(string& arguments)
{
    string opts;
    size_t pos = arguments.find(" -");

    if (pos != string::npos) {
        opts      = arguments.substr(pos);
        arguments = arguments.substr(0, pos);
    }

    return opts;
}

void Player::play_command(string arguments)
{
    // DEBUG
    //THEKERNEL->streams->printf("play_command: %s\r\n", arguments.c_str());

    // extract any options from the line and terminate the line there
    string options = extract_options(arguments);

    // Get file path which is the entire parameter line upto any options found or entire line
    if (! this->open_file(absolute_from_relative(arguments))) {
        this->output_stream->printf("File not found: %s\r\n", this->current_file.path.c_str());
        // TODO: clear file stack...
        return;
    }

    // start playing
    this->output_stream->printf("Playing %s\r\n", this->current_file.path.c_str());

    if (0 == this->current_file.size) {
        this->output_stream->printf("WARNING - Could not get file size\r\n");
    } else {
        this->output_stream->printf("File size %ld\r\n", this->current_file.size);
    }

    this->play_file();
}

void Player::progress_command(string arguments)
{
    // DEBUG
    //THEKERNEL->streams->printf("progress_command: %s\r\n", arguments.c_str());
}

void Player::abort_command(string arguments)
{
    // DEBUG
    //THEKERNEL->streams->printf("abort_command: %s\r\n", arguments.c_str());
}

void Player::suspend_command(string arguments)
{
    // DEBUG
    //THEKERNEL->streams->printf("suspend_command: %s\r\n", arguments.c_str());
}

void Player::resume_command(string arguments)
{
    // DEBUG
    //THEKERNEL->streams->printf("resume_command: %s\r\n", arguments.c_str());
}

// ON_GCODE_RECEIVED -----------------------------------------------------------

void Player::on_gcode_received(void *argument)
{
    // exit, if in halted state
    if (THEKERNEL->is_halted()) {
        return;
    }

    // get gcode from command line
    Gcode *gcode = static_cast<Gcode *>(argument);

    // exit if no G or M code
    if (!gcode->has_g && !gcode->has_m) return;

    // extract command line arguments
    string arguments = get_arguments(gcode->get_command());

    // set output stream
    //if (this->output_stream == NULL) {
        this->output_stream = gcode->stream;
    //}

    // DEBUG
    THEKERNEL->streams->printf("ON_GCODE_RECEIVED: %s\r\n", arguments.c_str());

    // Act depending on code
    if (gcode->has_g && gcode->g == 28) {
        this->G28(arguments);
    } else if (gcode->m == 21) {
        this->M21(arguments);
    } else if (gcode->m == 23) {
        this->M23(arguments);
    } else if (gcode->m == 24) {
        this->M24(arguments);
    } else if (gcode->m == 25) {
        this->M25(arguments);
    } else if (gcode->m == 26) {
        this->M26(arguments);
    } else if (gcode->m == 27) {
        this->M27(arguments);
    } else if (gcode->m == 32) {
        this->M32(arguments);
    } else if (gcode->m == 600) {
        this->M600(arguments);
    } else if (gcode->m == 601) {
        this->M601(arguments);
    }
}

// homing cancels suspend
void Player::G28(string arguments)
{
    // DEBUG
    //THEKERNEL->streams->printf("G28: %s\r\n", arguments.c_str());
}

// Dummy code; makes Octoprint happy -- supposed to initialize SD card
void Player::M21(string arguments)
{
    // DEBUG
    //THEKERNEL->streams->printf("M21: %s\r\n", arguments.c_str());
}

// open/select file
bool Player::M23(string arguments)
{
    // DEBUG
    //THEKERNEL->streams->printf("M23: %s\r\n", arguments.c_str());

    // try to open the file path
    if (this->open_file("/sd/" + arguments)) {
        this->output_stream->printf("File opened: %s Size: %ld\r\n", this->current_file.path.c_str(), this->current_file.size);
        this->output_stream->printf("File selected\r\n");
        return true;
    }

    this->output_stream->printf("open failed, File: %s\r\n", this->current_file.path.c_str());
    // TODO: clear file stack...
    return false;
}

// start print
void Player::M24(string arguments)
{
    // DEBUG
    //THEKERNEL->streams->printf("M24: %s\r\n", arguments.c_str());
    this->play_file();
}

// pause print
void Player::M25(string arguments)
{
    // DEBUG
    //THEKERNEL->streams->printf("M25: %s\r\n", arguments.c_str());
}

// reset print, Slightly different than M26 in Marlin and the rest
void Player::M26(string arguments)
{
    // DEBUG
    //THEKERNEL->streams->printf("M26: %s\r\n", arguments.c_str());
}

// report print progress, in format used by Marlin
void Player::M27(string arguments)
{
    // DEBUG
    //THEKERNEL->streams->printf("M27: %s\r\n", arguments.c_str());
}

// open/select file and start print
void Player::M32(string arguments)
{
    // DEBUG
    //THEKERNEL->streams->printf("M32: %s\r\n", arguments.c_str());

    // try to open/select file
    if (this->M23(arguments)) {
        this->M24(arguments); // play file
    }
}

// suspend print, Not entirely Marlin compliant, M600.1 will leave the heaters on
void Player::M600(string arguments)
{
    // DEBUG
    //THEKERNEL->streams->printf("M600: %s\r\n", arguments.c_str());
}

// resume print
void Player::M601(string arguments)
{
    // DEBUG
    //THEKERNEL->streams->printf("M601: %s\r\n", arguments.c_str());
}
