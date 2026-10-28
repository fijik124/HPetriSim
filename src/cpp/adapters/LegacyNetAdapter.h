#pragma once
#include "domain/PetriNet.h"

class CPetriSimDoc;

namespace hps::adapters
{
using ExportResult = std::variant<domain::NetSnapshot, std::vector<domain::ValidationIssue>>;

// Call on the document-owning thread while simulation is offline. This is an
// independent value copy, not synchronization for concurrent legacy mutation.
ExportResult ExportLegacyNet(CPetriSimDoc& document);
}
