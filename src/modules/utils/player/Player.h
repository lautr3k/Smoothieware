/*
This file is part of Smoothie (http://smoothieware.org/). The motion control part is heavily based on Grbl (https://github.com/simen/grbl).
Smoothie is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
Smoothie is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
You should have received a copy of the GNU General Public License along with Smoothie. If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "Module.h"

#include <string>
using std::string;

class StreamOutput;

struct file_info {
    string   path;
    FILE*    file;
    uint32_t size;
    uint32_t read;
};

class Player : public Module {
    public:
        Player();

        void on_module_loaded();

        void on_console_line_received(void* argument);
        void on_gcode_received(void *argument);
        void on_main_loop(void* argument);

    private:
        StreamOutput* output_stream;
        file_info     current_file;
        bool          playing;

        // file
        bool open_file(string path);
        void get_filesize();
        void play_file();
        void pause_file();
        void close_file();

        // console
        string extract_options(string& args);
        void play_command(string args);
        void progress_command(string args);
        void suspend_command(string args);
        void resume_command(string args);
        void abort_command(string args);

        // GMcode
        void G28(string args);
        void M21(string args);
        bool M23(string args);
        void M24(string args);
        void M25(string args);
        void M26(string args);
        void M27(string args);
        void M32(string args);
        void M600(string args);
        void M601(string args);
};
