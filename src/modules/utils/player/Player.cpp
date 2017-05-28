/*
This file is part of Smoothie (http://smoothieware.org/). The motion control part is heavily based on Grbl (https://github.com/simen/grbl).
Smoothie is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
Smoothie is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
You should have received a copy of the GNU General Public License along with Smoothie. If not, see <http://www.gnu.org/licenses/>.
*/

#include "mbed.h";
#include "Player.h"

#include "libs/Kernel.h"
#include "Gcode.h"
#include "libs/utils.h"
#include "libs/StreamOutput.h"
#include "libs/SerialMessage.h"
#include "libs/StreamOutputPool.h"

// #include "SDFAT.h"
// extern SDFAT mounter;


Player::Player() {
    this->reset_file("");
    this->file_handler = NULL;
}

void Player::on_module_loaded()
{
    this->register_for_event(ON_CONSOLE_LINE_RECEIVED);
    this->register_for_event(ON_GCODE_RECEIVED);
    this->register_for_event(ON_MAIN_LOOP);
}

// FILE HELPERS ----------------------------------------------------------------

void Player::reset_file(string path)
{
    this->file_path    = path;
    this->file_size    = 0;
    this->file_line    = 1;
    //this->file_handler = NULL;
    this->file_playing = false;
    this->file_paused  = false;
}

long int Player::get_filesize(FILE* &file)
{
    int result = fseek(file, 0, SEEK_END);

    if (result != 0) {
        return 0;
    }

    long int size = ftell(file);
    fseek(file, 0, SEEK_SET);
    return size;
}

void Player::close_file()
{
    if (this->file_handler != NULL) {
        fclose(this->file_handler);
        free(this->file_handler);
        wait(0.1); // ???
        this->file_handler = NULL;
    }
}

// return -1: file not found
// return  0: can not open file
// return  1: file opened
int Player::open_file(string path)
{
    if(! file_exists(path.c_str())) {
        return -1;
    }

    // close opened file
    //this->close_file();

    // reset file members
    this->reset_file(path);

    // (re)open file
    if (this->file_handler == NULL) {
        this->file_handler = fopen(path.c_str(), "r");
    }
    else {
        this->file_handler = freopen(path.c_str(), "r", this->file_handler);
    }

    //wait(0.1); // ???

    // can not open file
    if (this->file_handler == NULL) {
        return 0;
    }

    // get file size
    this->file_size = this->get_filesize(this->file_handler);

    return 1;
}

// from FileConfigSource::readLine
bool Player::readLine(string& line, int lineno, FILE *fp)
{
    char buf[132];
    char *l= fgets(buf, sizeof(buf)-1, fp);
    if(l != NULL) {
        if(buf[strlen(l)-1] != '\n') {
            // truncate long lines
            if(lineno != 0) {
                // report if it is not truncating a comment
                if(strchr(buf, '#') == NULL){
                    // DEBUG
                    THEKERNEL->streams->printf("Truncated long line %d in: %s\n", lineno, this->file_path.c_str());
                }
            }
            // read until the next \n or eof
            int c;
            while((c=fgetc(fp)) != '\n' && c != EOF) /* discard */;
        }
        line.assign(buf);
        return true;
    }

    return false;
}

// ON_CONSOLE_LINE_RECEIVED ----------------------------------------------------

void Player::on_console_line_received(void* argument)
{
    // exit, if in halted state
    if (THEKERNEL->is_halted()) {
        return;
    }

    // get command line from (serial) message
    SerialMessage serial    = *static_cast<SerialMessage *>(argument);
    string        arguments = serial.message;

    // ignore anything that is not lowercase or a letter
    if (arguments.empty() || !islower(arguments[0]) || !isalpha(arguments[0])) {
        return;
    }

    // extract command name
    string cmd = shift_parameter(arguments);

    // Act depending on command
    if (cmd == "play") {
        this->play_command(arguments);
    }
}

// extract any options found on line, terminates arguments at the space before the first option (-v)
// eg this is a file.gcode -v will return -v and set arguments to this is a file.gcode
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
    // extract any options from the line and terminate the line there
    string options = extract_options(arguments);
    string path    = absolute_from_relative(arguments);

    // DEBUG
    THEKERNEL->streams->printf("PLAY: %s\r\n", path.c_str());

    // open and play file
    int result = this->open_file(path);

    if (result == 1) {
        this->file_playing = true;
    }
    else if (result == 0) {
        // DEBUG
        THEKERNEL->streams->printf("can not open file: %s\r\n", path.c_str());
    }
    else if (result == -1) {
        // DEBUG
        THEKERNEL->streams->printf("file not found: %s\r\n", path.c_str());
    }
}

// ON_GCODE_RECEIVED -----------------------------------------------------------

void Player::on_gcode_received(void *argument)
{
    // exit, if in halted state
    if (THEKERNEL->is_halted()) {
        return;
    }
}

// ON_MAIN_LOOP ----------------------------------------------------------------

void Player::on_main_loop(void* argument)
{
    // exit, if in halted state
    if (THEKERNEL->is_halted()) {
        return;
    }

    // exit, not playing file
    if (! this->file_playing) {
        return;
    }

    // For each line
    while(! feof(this->file_handler)) {
        string line;

        // break loop, if an error occured reading the line
        if (! this->readLine(line, this->file_line++, this->file_handler)) {
            break;
        }

        // send line as serial message
        struct SerialMessage message;

        message.message = line;
        message.stream  = &(StreamOutput::NullStream);

        THEKERNEL->call_event(ON_CONSOLE_LINE_RECEIVED, &message);

        // exit main loop
        return;
    }

    // play done
    this->file_playing = false;

    // DEBUG
    THEKERNEL->streams->printf("play done !\r\n");
}
