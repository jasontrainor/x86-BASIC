#ifndef C64LISTING_H
#define C64LISTING_H

// Listings as text (listing.c): whether one is written with lower case
// letters (as tools write them: then capitals are shifted), and one line
// of it as PETSCII into out (up to cap - 1 characters); its length.
int c64_listing_lower(const char *text, int len);
int c64_listing_line(const char *s, int len, char *out, int cap, int lower);

// The disk and the other devices (disk.c), for basic.c.
void disk_init(void);
int disk_statement(int token);            // LOAD, SAVE, OPEN ...: 0 if not one
int disk_out(int c);                      // a character printed: 1 if it went to a file
int disk_to_file(void);                   // printing is going to a file (not the screen)
int disk_input_line(char *buf, int cap, int *done);   // INPUT#: *done if it read
int disk_get(int *done);                  // GET#: the character (-1: none)
void disk_close_all(void);                // (CLR, RUN ... close the files)
void disk_screen(void);                   // output back to the screen (after an error)

#endif
