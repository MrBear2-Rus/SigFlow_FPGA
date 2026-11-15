#pragma once
#include "VirtualSession.h"
#include "PortManifestBuilder.h"
namespace eda::virtual_board {
// Config-driven production JSONL entry used by every generated module.
int SessionRunnerMain(int argc,char** argv,const PortManifest&,VirtualSession::Factory);
}
