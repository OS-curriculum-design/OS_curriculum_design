#ifndef SHELL_H
#define SHELL_H

void shell_init(void);
void shell_prompt(void);
void shell_handle_char(char c);
void shell_begin_async_output(void);
void shell_note_async_output(void);
void shell_end_async_output(void);

#endif
