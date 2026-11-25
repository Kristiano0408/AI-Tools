#pragma once

#include "../parser.h"

namespace parser {

// Parse CUDA specific constructs (reads the file from disk)
FileMetadata parse_cuda(const std::string& file_path);

// Parse CUDA from an already-read byte snapshot (IDX-03): the file is not
// reopened, so the caller's hash and the parsed symbols describe one version.
FileMetadata parse_cuda(const std::string& file_path, const std::string& content);

}
