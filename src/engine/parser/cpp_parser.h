#pragma once

#include "../parser.h"

namespace parser {

// Parse C/C++ specific constructs
FileMetadata parse_cpp(const std::string& file_path);

}
