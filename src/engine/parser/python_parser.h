#pragma once

#include "../parser.h"

namespace parser {

// Parse Python specific constructs
FileMetadata parse_python(const std::string& file_path);

}
