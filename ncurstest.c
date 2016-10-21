#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ncurses.h>

#define LCD_ROWS	 4
#define LCD_COLS	20

#define BORDER_CLR_ID	 1
#define SCREEN_CLR_ID	 2

void drawFrame(int row, int col, int height, int width)
{
	int i;
	mvaddch(row, col, '+');
	hline('-', width - 2);
	mvaddch(row, col + width - 1, '+');
	mvaddch(row + height - 1, col, '+');
	hline('-', width - 2);
	mvaddch(row + height - 1, col + width - 1, '+');
	mvvline(row + 1, col, '|', height - 2);
	mvvline(row + 1, col + width - 1, '|', height - 2);
}

int main(int argc, char *argv[])
{
	int i, xs, ys, xw, yw;

	initscr();
	cbreak();
	noecho();
	curs_set(0);
	start_color();
	init_pair(BORDER_CLR_ID, COLOR_BLACK, COLOR_GREEN);
	init_pair(SCREEN_CLR_ID, COLOR_WHITE, COLOR_BLUE);
	getmaxyx(stdscr, ys, xs);
	yw = (ys - LCD_ROWS) >> 1;
	xw = (xs - LCD_COLS) >> 1;
	attron(COLOR_PAIR(BORDER_CLR_ID));
	drawFrame(yw, xw, LCD_ROWS + 2, LCD_COLS + 2);
	attroff(COLOR_PAIR(BORDER_CLR_ID));
	attron(COLOR_PAIR(SCREEN_CLR_ID));
	for (i = 0; i < LCD_ROWS; i++)
		if (i < argc - 1)
			mvprintw(yw + 1 + i, xw + 1, "%-*s", LCD_COLS, argv[i + 1]);
		else
			mvprintw(yw + 1 + i, xw + 1, "%*s", LCD_COLS, " ");
	attroff(COLOR_PAIR(SCREEN_CLR_ID));
	refresh();
	getch();
	endwin();
	return 0;
}
