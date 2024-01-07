"""
@@@  @@@@@@@    @@@@@@@     @@@  @@@  @@@   @@@@@@   @@@@@@@    @@@@@@
@@@  @@@@@@@@  @@@@@@@@     @@@  @@@  @@@  @@@@@@@@  @@@@@@@@  @@@@@@@
@@!  @@!  @@@  !@@          @@!  @@!  @@!  @@!  @@@  @@!  @@@  !@@
!@!  !@!  @!@  !@!          !@!  !@!  !@!  !@!  @!@  !@!  @!@  !@!
!!@  @!@!!@!   !@!          @!!  !!@  @!@  @!@!@!@!  @!@!!@!   !!@@!!
!!!  !!@!@!    !!!          !@!  !!!  !@!  !!!@!!!!  !!@!@!     !!@!!!
!!:  !!: :!!   :!!          !!:  !!:  !!:  !!:  !!!  !!: :!!        !:!
:!:  :!:  !:!  :!:          :!:  :!:  :!:  :!:  !:!  :!:  !:!      !:!
 ::  ::   :::   ::: :::      :::: :: :::   ::   :::  ::   :::  :::: ::
:     :   : :   :: :: :       :: :  : :     :   : :   :   : :  :: : :

         ##      ##                           ############
       ##############                     ####################
     ####  ######  ####     <<=======   ########################
   ######################             ####  ####  ####  ####  ####
   ##  ##############  ##  >>>>>)    ##############################
   ##  ##          ##  ##   <(====     #######    ####    #######
         ####  ####                      ###                ###

    ------------------------------------------------------------
     Simple IRCd Configuration Editor (SICE)
    ------------------------------------------------------------
     SICE is a lightweight and user-friendly text editor
     specifically designed for editing .conf files for ircd.
     It enhances readability and ease of editing by color-coding
     the separators, making it easier to navigate through
     configuration files.

     Features:
     - Customizable separator coloring for improved readability.
     - Support for basic text editing operations.
     - Status bar displaying file name and current line position.

     Usage:
     Run the program in a Linux terminal with the following
         command:

        python3 editor.py [-s SEPARATOR] filename

     Arguments:
     - '-s SEPARATOR': Optional. Specifies a custom separator
       character for coloring. If not provided, '%' is used.
     - 'filename': The path to the .conf file to be edited.

     Example:
        python3 editor.py -s ":" myconfig.conf

     Controls:
     - Arrow keys to navigate.
     - Page Up/Page Down to scroll.
     - Ctrl+X to exit (prompts to save changes).
    ------------------------------------------------------------
"""
import curses
import sys
import argparse

def parse_args():
    parser = argparse.ArgumentParser(description="Text editor with custom separator")
    parser.add_argument("-s", "--separator", default="%", help="Separator character")
    parser.add_argument("filename", help="File to edit")
    return parser.parse_args()

def draw_status_bar(stdscr, filename, current_line, total_lines):
    status = f"File: {filename} - Line: {current_line + 1}/{total_lines} - For exit press Ctrl + X"
    stdscr.attron(curses.color_pair(3))
    stdscr.addstr(curses.LINES - 1, 0, status)
    stdscr.addstr(curses.LINES - 1, len(status), " " * (curses.COLS - len(status) - 1))
    stdscr.attroff(curses.color_pair(3))

def main(stdscr, filename, separator):
    curses.use_default_colors()
    curses.init_pair(1, curses.COLOR_CYAN, -1)  # Cyan for comments
    curses.init_pair(2, curses.COLOR_MAGENTA, -1)  # Pink for separator
    curses.init_pair(3, curses.COLOR_WHITE, curses.COLOR_BLUE)  # Status bar
    curses.init_pair(4, curses.COLOR_RED, -1)  # Red for the first char
    curses.init_pair(5, curses.COLOR_WHITE, -1)  # Light gray for normal text

    try:
        with open(filename, 'r') as file:
            lines = file.readlines()
    except FileNotFoundError:
        with open(filename, 'w') as file:
            file.write('')
        lines = ['']

    current_line = 0
    current_col = 0
    scroll_offset = 0

    while True:
        stdscr.clear()

        # Draw status bar
        draw_status_bar(stdscr, filename, current_line, len(lines))

        # Display the file
        for i in range(scroll_offset, min(scroll_offset + curses.LINES - 1, len(lines))):
            line = lines[i]
            if line.startswith('#'):
                stdscr.addstr(i - scroll_offset, 0, line, curses.color_pair(1))
            else:
                if line:
                    stdscr.addstr(i - scroll_offset, 0, line[0], curses.color_pair(4))
                pos = 1
                while pos < len(line):
                    if line[pos] == separator:
                        stdscr.addstr(i - scroll_offset, pos, separator, curses.color_pair(2))
                    else:
                        stdscr.attron(curses.color_pair(5))
                        stdscr.addstr(i - scroll_offset, pos, line[pos])
                        stdscr.attroff(curses.color_pair(5))
                    pos += 1

        # Move the cursor
        stdscr.move(min(current_line - scroll_offset, curses.LINES - 2), current_col)

        # Handle input
        key = stdscr.getch()
        if key == ord('\x18'):  # Ctrl+X
            stdscr.addstr(curses.LINES - 2, 0, "Save changes? (y/n) ")
            answer = stdscr.getch()
            if answer in [ord('y'), ord('Y')]:
                with open(filename, 'w') as file:
                    file.writelines(lines)
            break
        elif key == curses.KEY_UP:
            if current_line > 0:
                current_line -= 1
                if current_line < scroll_offset:
                    scroll_offset -= 1
        elif key == curses.KEY_DOWN:
            if current_line < len(lines) - 1:
                current_line += 1
                if current_line - scroll_offset >= curses.LINES - 1:
                    scroll_offset += 1
        elif key == curses.KEY_LEFT:
            if current_col > 0:
                current_col -= 1
        elif key == curses.KEY_RIGHT:
            if current_col < len(lines[current_line]):
                current_col += 1
        elif key == curses.KEY_NPAGE:  # Page Down
            if scroll_offset + curses.LINES - 1 < len(lines) - 1:
                scroll_offset += curses.LINES - 1
                current_line = min(current_line + curses.LINES - 1, len(lines) - 1)
        elif key == curses.KEY_PPAGE:  # Page Up
            if scroll_offset > 0:
                scroll_offset = max(scroll_offset - curses.LINES + 1, 0)
                current_line = max(current_line - curses.LINES + 1, 0)
        elif key == curses.KEY_ENTER or key in [10, 13]:
            new_line = lines[current_line][current_col:]
            lines[current_line] = lines[current_line][:current_col]
            lines.insert(current_line + 1, new_line)
            current_line += 1
            current_col = 0
        elif key in [curses.KEY_BACKSPACE, 127]:
            if current_col > 0:
                lines[current_line] = lines[current_line][:current_col - 1] + lines[current_line][current_col:]
                current_col -= 1
            elif current_line > 0:
                current_line -= 1
                current_col = len(lines[current_line])
                lines[current_line] = lines[current_line].rstrip('\n') + lines.pop(current_line + 1)
        elif key in range(32, 127):
            lines[current_line] = lines[current_line][:current_col] + chr(key) + lines[current_line][current_col:]
            current_col += 1

        stdscr.refresh()

if __name__ == "__main__":
    args = parse_args()
    curses.wrapper(main, args.filename, args.separator)
