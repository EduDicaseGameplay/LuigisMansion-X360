// Log to game:\lm360.log (Xbox 360) or lm360.log (PC host tests). The first line is the build version:
// check it before reading a test (lesson BRE/Strike: tests ran with an old XEX).
#pragma once

void lm_log_open(const char* path, const char* version);
void lm_log(const char* fmt, ...);
void lm_log_once(unsigned key, const char* fmt, ...);
void lm_fatal(const char* fmt, ...);
void lm_log_flush();  // writes the buffered lines now (fatal errors, exit)
void lm_log_kick();   // asks the writer thread to write them soon
