// "Report a problem": one zip with what a player can send back after a crash,
// freeze or graphics glitch -- their description, PC info, settings, the last
// game sessions' logs and that session's F9 frame captures.
#pragma once
#include <string>

// Writes <install>\logs\bug_report_<date>_<time>.zip. Returns "" on success
// (path in *zip_out), else what went wrong.
std::string make_bug_report(const std::wstring& install_dir, const std::string& note, std::wstring* zip_out);
