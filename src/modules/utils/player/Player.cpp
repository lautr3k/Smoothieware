/*
This file is part of Smoothie (http://smoothieware.org/). The motion control part is heavily based on Grbl (https://github.com/simen/grbl).
Smoothie is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
Smoothie is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
You should have received a copy of the GNU General Public License along with Smoothie. If not, see <http://www.gnu.org/licenses/>.
*/

#include "Player.h"

#include "mbed.h"
#include "Gcode.h"
#include "SDFAT.h"

#include "libs/utils.h"
#include "libs/Kernel.h"
#include "libs/StreamOutput.h"
#include "libs/SerialMessage.h"
#include "libs/StreamOutputPool.h"

extern SDFAT mounter;

Player::Player() {
    this->file_handler = NULL;
    this->reset_file("", 0, 0, 0); // path, size, line, read
}

void Player::on_module_loaded()
{
    this->register_for_event(ON_CONSOLE_LINE_RECEIVED);
    this->register_for_event(ON_GCODE_RECEIVED);
    this->register_for_event(ON_MAIN_LOOP);
}

// FILE HELPERS ----------------------------------------------------------------

void Player::reset_file(string path, long int size, int line, long int read)
{
    this->file_path    = path;
    this->file_size    = size;
    this->file_line    = line;
    this->file_read    = read;
    this->file_playing = false;
    this->file_paused  = false;
}

void Player::get_file_size()
{
    if (fseek(this->file_handler, 0, SEEK_END) != 0) {
        this->file_size = 0;
    } else {
        this->file_size = ftell(this->file_handler);
        fseek(this->file_handler, 0, SEEK_SET);
    }
}

// return -1: file not found
// return  0: can not open file
// return  1: file opened
int Player::open_file(string path)
{
    // file not found
    if (! file_exists(path.c_str())) {
        return -1;
    }

    if (this->file_playing) {
        // If a file is already playing,
        // we need to store it before trying to access a new one
        this->file_stack.push_back({
            this->file_path, this->file_size, this->file_line, this->file_read
        });

        // DEBUG
        // THEKERNEL->streams->printf("---> path: %s, size: %li, line: %d\r\n", this->file_path.c_str(), this->file_size, this->file_line);
        // THEKERNEL->streams->printf("---> stack size: %i\r\n", this->file_stack.size());
    }

    // reset file members
    this->reset_file(path, 0, 0, 0); // path, size, line, read

    // (re)open file
    if (this->file_handler == NULL) {
        this->file_handler = fopen(path.c_str(), "r");
    }
    else {
        this->file_handler = freopen(path.c_str(), "r", this->file_handler);
    }

    // can not open file
    if (this->file_handler == NULL) {
        return 0;
    }

    // get file size
    this->get_file_size();

    return 1;
}

// from FileConfigSource::readLine
bool Player::read_file_line(string& line)
{
    if (this->file_handler == NULL || feof(this->file_handler)) {
        return false;
    }

    char buf[132];

    char *new_line = fgets(buf, sizeof(buf) - 1, this->file_handler);

    if (new_line == NULL) {
        return false;
    }

    int len = strlen(new_line);

    this->file_read += len;
    this->file_line++;

    // truncate long lines
    if (buf[len - 1] != '\n') {
        // report if it is not truncating a comment
        if (strchr(buf, ';') == NULL || strchr(buf, '(') == NULL) {
            THEKERNEL->streams->printf("Truncated long line %d in: %s\n", this->file_line, this->file_path.c_str());
        }

        // read until the next \n or eof
        int c;
        while ((c = fgetc(this->file_handler)) != '\n' && c != EOF) /* discard */;
    }

    line.assign(buf);
    return true;
}

void Player::play_file()
{
    this->file_playing = true;
    this->file_paused  = false;
}

void Player::pause_file()
{
    this->file_playing = false;
    this->file_paused  = true;
}

void Player::stop_file()
{
    this->file_playing = false;
    this->file_paused  = false;
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

    // Act depending on command name
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
        this->play_file();
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

    // get gcode arguments
    Gcode *gcode     = static_cast<Gcode *>(argument);
    string arguments = get_arguments(gcode->get_command());

    // Act depending on gcode
    if (gcode->has_m) {
        if (gcode->m == 23) {
            this->M23(arguments); // open file
        } else if (gcode->m == 24) {
            this->M24(arguments); // play file
        } else if (gcode->m == 32) {
            this->M32(arguments); // open and play file
        }
    }
}

bool Player::M23(string args)
{
    // open and play file
    int result = this->open_file("/sd/" + args);

    if (result == 1) {
        return true;
    }

    if (result == 0) {
        // DEBUG
        THEKERNEL->streams->printf("can not open file: %s\r\n", this->file_path.c_str());
    }
    else if (result == -1) {
        // DEBUG
        THEKERNEL->streams->printf("file not found: %s\r\n", this->file_path.c_str());
    }

    return false;
}

void Player::M24(string args)
{
    this->play_file();
}

void Player::M32(string args)
{
    if (this->M23(args)) {
        this->M24(args);
    }
}

// ON_MAIN_LOOP ----------------------------------------------------------------

void Player::on_main_loop(void* argument)
{
    // exit, if in halted state
    if (THEKERNEL->is_halted()) {
        return;
    }

    // exit, if not playing file
    if (! this->file_playing || this->file_handler == NULL) {
        return;
    }

    // get next line
    string line;

    if (this->read_file_line(line)) {
        // send line as serial message
        struct SerialMessage message;

        message.message = line.c_str();
        message.stream  = THEKERNEL->streams;

        THEKERNEL->call_event(ON_CONSOLE_LINE_RECEIVED, &message);

        // exit main loop
        return;
    }

    // pause file
    this->pause_file();

    // If there is a file in the stack
    if (! this->file_stack.empty()) {
        // pop it and resume playing it
        stacked_file file = this->file_stack.back();
        this->file_stack.pop_back();

        // DEBUG
        // THEKERNEL->streams->printf("<--- path: %s, size: %li, line: %d\r\n", file.path.c_str(), file.size, file.line);
        // THEKERNEL->streams->printf("<--- stack size: %i\r\n", this->file_stack.size());

        // remember parent file
        this->reset_file(file.path, file.size, file.line, file.read);

        // (re)open file
        this->file_handler = freopen(file.path.c_str(), "r", this->file_handler);

        // can not open file
        if (this->file_handler == NULL) {
            // DEBUG
            THEKERNEL->streams->printf("can not open file: %s\r\n", file.path.c_str());
            return;
        }

        // remember file position
        fseek(this->file_handler, this->file_read, SEEK_SET);

        // play file
        this->play_file();

        // exit main loop
        return;
    }

    // play done
    fclose(this->file_handler);
    this->stop_file();

    // DEBUG
    THEKERNEL->streams->printf("play done !\r\n");
}
