#pragma once

#include "../parser.h"

namespace parser {

// Parse CUDA specific constructs
FileMetadata parse_cuda(const std::string& file_path);

}
