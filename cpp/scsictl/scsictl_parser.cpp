//---------------------------------------------------------------------------
//
// SCSI Target Emulator PiSCSI
// for Raspberry Pi
//
// Copyright (C) 2022 Uwe Seimet
//
//---------------------------------------------------------------------------

#include "scsictl_parser.h"

PbOperation ScsictlParser::ParseOperation(const string& operation) const
{
	const auto& it = operations.find(tolower(operation[0]));
	return it != operations.end() ? it->second : NO_OPERATION;
}

PbDeviceType ScsictlParser::ParseType(const string& type) const
{
	string t = type;
	transform(t.begin(), t.end(), t.begin(), ::toupper);

	if (PbDeviceType parsed_type; PbDeviceType_Parse(t, &parsed_type)) {
		return parsed_type;
	}

	// Handle convenience device types (shortcuts)
	const auto& it = device_types.find(tolower(type[0]));
	return it != device_types.end() ? it->second : UNDEFINED;
}
