//---------------------------------------------------------------------------
//
//	SCSI Target Emulator PiSCSI
//	for Raspberry Pi
//
//	Powered by XM6 TypeG Technology.
//	Copyright (C) 2016-2020 GIMONS
//	Copyright (C) 2020-2023 Contributors to the PiSCSI project
//
//---------------------------------------------------------------------------

#include "shared/config.h"
#include "shared/log.h"
#include "shared/piscsi_util.h"
#include "shared/protobuf_serializer.h"
#include "shared/protobuf_util.h"
#include "shared/piscsi_exceptions.h"
#include "shared/piscsi_version.h"
#include "controllers/controller_manager.h"
#include "controllers/scsi_controller.h"
#include "devices/device_factory.h"
#include "devices/storage_device.h"
#include "hal/gpiobus_factory.h"
#include "hal/gpiobus.h"
#include "hal/systimer.h"
#include "piscsi/piscsi_executor.h"
#include "piscsi/piscsi_core.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include <netinet/in.h>
#include <csignal>
#include <string>
#include <sstream>
#include <iostream>
#include <fstream>
#include <list>
#include <filesystem>

using namespace std;
using namespace filesystem;
using namespace spdlog;
using namespace piscsi_interface;
using namespace piscsi_util;
using namespace protobuf_util;
using namespace scsi_defs;

void Piscsi::Banner(const vector<char *>& args) const
{
	cout << piscsi_util::Banner("(Backend Service)");
	cout << "Connection type: " << CONNECT_DESC << '\n' << flush;

	if ((args.size() > 1 && strcmp(args[1], "-h") == 0) || (args.size() > 1 && strcmp(args[1], "--help") == 0)){
		cout << "\nUsage: " << args[0] << " [-idID[:LUN] FILE] ...\n\n";
		cout << " ID is SCSI device ID (0-7).\n";
		cout << " LUN is the optional logical unit (0-31).\n";
		cout << " FILE is a disk image file, \"daynaport\", \"bridge\", \"printer\" or \"services\".\n\n";
		cout << " Image type is detected based on file extension if no explicit type is specified.\n";
		cout << "  hd1 : SCSI-1 HD image (Non-removable generic SCSI-1 HD image)\n";
		cout << "  hds : SCSI HD image (Non-removable generic SCSI HD image)\n";
		cout << "  hdr : SCSI HD image (Removable generic HD image)\n";
		cout << "  hda : SCSI HD image (Apple compatible image)\n";
		cout << "  hdn : SCSI HD image (NEC compatible image)\n";
		cout << "  hdi : SCSI HD image (Anex86 HD image)\n";
		cout << "  nhd : SCSI HD image (T98Next HD image)\n";
		cout << "  mos : SCSI MO image (MO image)\n";
		cout << "  iso : SCSI CD image (ISO 9660 image)\n" << flush;

		exit(EXIT_SUCCESS);
	}
}

bool Piscsi::InitBus() const
{
	bus = GPIOBUS_Factory::Create(BUS::mode_e::TARGET);
	if (bus == nullptr) {
		return false;
	}

	auto b = bus;
	controller_manager = make_shared<ControllerManager>(*b);
	auto c = controller_manager;
	executor = make_shared<PiscsiExecutor>(piscsi_image, *c);

	return true;
}

void Piscsi::Cleanup()
{
	executor->DetachAll();

	service.Cleanup();

	bus->Cleanup();
}

void Piscsi::ReadAccessToken(const string& filename) const
{
	struct stat st;
	if (stat(filename.c_str(), &st) || !S_ISREG(st.st_mode)) {
		throw parser_exception("Can't access token file '" + filename + "'");
	}

	if (st.st_uid || st.st_gid) {
		throw parser_exception("Access token file '" + filename + "' must be owned by root");
	}

	if (const auto perms = filesystem::status(filename).permissions();
		(perms & perms::group_read) != perms::none || (perms & perms::others_read) != perms::none ||
			(perms & perms::group_write) != perms::none || (perms & perms::others_write) != perms::none) {
		throw parser_exception("Access token file '" + filename + "' must be readable by root only");
	}

	ifstream token_file(filename);
	if (token_file.fail()) {
		throw parser_exception("Can't open access token file '" + filename + "'");
	}

	getline(token_file, access_token);
	if (token_file.fail()) {
		throw parser_exception("Can't read access token file '" + filename + "'");
	}

	if (access_token.empty()) {
		throw parser_exception("Access token file '" + filename + "' must not be empty");
	}
}

void Piscsi::LogDevices(string_view devices) const
{
	stringstream ss(devices.data());
	string line;

	while (getline(ss, line, '\n')) {
		LOGINFO("%s", line.c_str())
	}
}

PbDeviceType Piscsi::ParseDeviceType(const string& value) const
{
	string t = value;
	PbDeviceType type;
	transform(t.begin(), t.end(), t.begin(), ::toupper);
	if (!PbDeviceType_Parse(t, &type)) {
		throw parser_exception("Illegal device type '" + value + "'");
	}

	return type;
}

void Piscsi::TerminationHandler(int)
{
	Cleanup();

	// Process will terminate automatically
}

Piscsi::optargs_type Piscsi::ParseArguments(const vector<char *>& args, int& port) const
{
	optargs_type optargs;
	int block_size = 0;
	string name;

	opterr = 1;
	int opt;
	while ((opt = getopt(static_cast<int>(args.size()), args.data(), "-Iib:d:n:p:r:t:z:D:F:L:P:R:C:v")) != -1) {
		switch (opt) {
			// The following options can not be processed until AFTER
			// the 'bus' object is created and configured
			case 'i':
			case 'I':
			case 'd':
			case 'D':
			case 'R':
			case 'n':
			case 'r':
			case 't':
			case 'F':
			case 'z':
			{
				const string optarg_str = optarg == nullptr ? "" : optarg;
				optargs.emplace_back(opt, optarg_str);
				continue;
			}

			case 'b': {
				if (!GetAsUnsignedInt(optarg, block_size)) {
					throw parser_exception("Invalid block size " + string(optarg));
				}
				continue;
			}

			case 'L':
				current_log_level = optarg;
				continue;

			case 'p':
				if (!GetAsUnsignedInt(optarg, port) || port <= 0 || port > 65535) {
					throw parser_exception("Invalid port " + string(optarg) + ", port must be between 1 and 65535");
				}
				continue;

			case 'P':
				ReadAccessToken(optarg);
				continue;

			case 'v':
				cout << piscsi_get_version_string() << endl;
				exit(0);

			case 1:
			{
				// Encountered filename
				const string optarg_str = (optarg == nullptr) ? "" : string(optarg);
				optargs.emplace_back(opt, optarg_str);
				continue;
			}

			default:
				throw parser_exception("Parser error");
		}

		if (optopt) {
			throw parser_exception("Praser error");
		}
	}

	return optargs;
}

void Piscsi::CreateInitialDevices(const optargs_type& optargs) const
{
	PbCommand command;
	PbDeviceType type = UNDEFINED;
	int block_size = 0;
	string name;
	string log_level;
	string id_and_lun;

	const char *locale = setlocale(LC_MESSAGES, "");
	if (locale == nullptr || !strcmp(locale, "C")) {
		locale = "en";
	}

	opterr = 1;
	for (const auto& [option, value] : optargs) {
		switch (option) {
			case 'i':
			case 'I':
				continue;

			case 'd':
			case 'D':
				id_and_lun = value;
				continue;

			case 'z':
				locale = value.c_str();
				continue;

			case 'F':
				if (const string error = piscsi_image.SetDefaultFolder(value); !error.empty()) {
					throw parser_exception(error);
				}
				continue;

			case 'R':
				int depth;
				if (!GetAsUnsignedInt(value, depth)) {
					throw parser_exception("Invalid image file scan depth " + value);
				}
				piscsi_image.SetDepth(depth);
				continue;

			case 'n':
				name = value;
				continue;

			case 'r':
				if (const string error = executor->SetReservedIds(value); !error.empty()) {
					throw parser_exception(error);
				}
				continue;

			case 't':
				type = ParseDeviceType(value);
				continue;

			case 1:
				// Encountered filename
				break;

			default:
				throw parser_exception("Parser error");
		}

		PbDeviceDefinition *device = command.add_devices();

		if (!id_and_lun.empty()) {
			if (const string error = SetIdAndLun(*device, id_and_lun, ScsiController::LUN_MAX); !error.empty()) {
				throw parser_exception(error);
			}
		}

		device->set_type(type);
		device->set_block_size(block_size);

		ParseParameters(*device, value);

		SetProductData(*device, name);

		type = UNDEFINED;
		block_size = 0;
		name = "";
		id_and_lun = "";
	}

	// Attach all specified devices
	command.set_operation(ATTACH);

	if (CommandContext context(locale); !executor->ProcessCmd(context, command)) {
		throw parser_exception("Can't execute " + PbOperation_Name(command.operation()));
	}

	// Display and log the device list
	PbServerInfo server_info;
	piscsi_response.GetDevices(controller_manager->GetAllDevices(), server_info, piscsi_image.GetDefaultFolder());
	const list<PbDevice>& devices = { server_info.devices_info().devices().begin(), server_info.devices_info().devices().end() };
	const string device_list = ListDevices(devices);
	LogDevices(device_list);
	cout << device_list << flush;
}

bool Piscsi::ExecuteCommand(const CommandContext& context, const PbCommand& command)
{
	if (!access_token.empty() && access_token != GetParam(command, "token")) {
		return context.ReturnLocalizedError(LocalizationKey::ERROR_AUTHENTICATION, UNAUTHORIZED);
	}

	if (!PbOperation_IsValid(command.operation())) {
		LOGERROR("Received unknown command with operation opcode %d", command.operation())

		return context.ReturnLocalizedError(LocalizationKey::ERROR_OPERATION, UNKNOWN_OPERATION);
	}

	LOGTRACE("Received %s command", PbOperation_Name(command.operation()).c_str())

	PbResult result;
	ProtobufSerializer serializer;

	switch(command.operation()) {
		case LOG_LEVEL: {
			const string log_level = GetParam(command, "level");
			if (const bool status = executor->SetLogLevel(log_level); !status) {
				context.ReturnLocalizedError(LocalizationKey::ERROR_LOG_LEVEL, log_level);
			}
			else {
				current_log_level = log_level;

				context.ReturnStatus();
			}
			break;
		}

		case DEFAULT_FOLDER: {
			if (const string status = piscsi_image.SetDefaultFolder(GetParam(command, "folder")); !status.empty()) {
				context.ReturnStatus(false, status);
			}
			else {
				context.ReturnStatus();
			}
			break;
		}

		case DEVICES_INFO: {
			piscsi_response.GetDevicesInfo(controller_manager->GetAllDevices(), result, command,
					piscsi_image.GetDefaultFolder());
			serializer.SerializeMessage(context.GetFd(), result);
			break;
		}

		case DEVICE_TYPES_INFO: {
			result.set_allocated_device_types_info(piscsi_response.GetDeviceTypesInfo(result).release());
			serializer.SerializeMessage(context.GetFd(), result);
			break;
		}

		case SERVER_INFO: {
			result.set_allocated_server_info(piscsi_response.GetServerInfo(controller_manager->GetAllDevices(),
					result, executor->GetReservedIds(), current_log_level, piscsi_image.GetDefaultFolder(),
					GetParam(command, "folder_pattern"), GetParam(command, "file_pattern"),
					piscsi_image.GetDepth()).release());
			serializer.SerializeMessage(context.GetFd(), result);
			break;
		}

		case VERSION_INFO: {
			result.set_allocated_version_info(piscsi_response.GetVersionInfo(result).release());
			serializer.SerializeMessage(context.GetFd(), result);
			break;
		}

		case LOG_LEVEL_INFO: {
			result.set_allocated_log_level_info(piscsi_response.GetLogLevelInfo(result, current_log_level).release());
			serializer.SerializeMessage(context.GetFd(), result);
			break;
		}

		case DEFAULT_IMAGE_FILES_INFO: {
			result.set_allocated_image_files_info(piscsi_response.GetAvailableImages(result,
					piscsi_image.GetDefaultFolder(), GetParam(command, "folder_pattern"),
					GetParam(command, "file_pattern"), piscsi_image.GetDepth()).release());
			serializer.SerializeMessage(context.GetFd(), result);
			break;
		}

		case IMAGE_FILE_INFO: {
			if (string filename = GetParam(command, "file"); filename.empty()) {
				context.ReturnLocalizedError( LocalizationKey::ERROR_MISSING_FILENAME);
			}
			else {
				auto image_file = make_unique<PbImageFile>();
				const bool status = piscsi_response.GetImageFile(*image_file.get(), piscsi_image.GetDefaultFolder(), filename);
				if (status) {
					result.set_status(true);
					result.set_allocated_image_file_info(image_file.get());
					serializer.SerializeMessage(context.GetFd(), result);
				}
				else {
					context.ReturnLocalizedError(LocalizationKey::ERROR_IMAGE_FILE_INFO);
				}
			}
			break;
		}

		case NETWORK_INTERFACES_INFO: {
			result.set_allocated_network_interfaces_info(piscsi_response.GetNetworkInterfacesInfo(result).release());
			serializer.SerializeMessage(context.GetFd(), result);
			break;
		}

		case MAPPING_INFO: {
			result.set_allocated_mapping_info(piscsi_response.GetMappingInfo(result).release());
			serializer.SerializeMessage(context.GetFd(), result);
			break;
		}

		case OPERATION_INFO: {
			result.set_allocated_operation_info(piscsi_response.GetOperationInfo(result,
					piscsi_image.GetDepth()).release());
			serializer.SerializeMessage(context.GetFd(), result);
			break;
		}

		case RESERVED_IDS_INFO: {
			result.set_allocated_reserved_ids_info(piscsi_response.GetReservedIds(result,
					executor->GetReservedIds()).release());
			serializer.SerializeMessage(context.GetFd(), result);
			break;
		}

		case SHUT_DOWN: {
			if (executor->ShutDown(context, GetParam(command, "mode"))) {
				TerminationHandler(0);
			}
			break;
		}

		default: {
			// Wait until we become idle
			const timespec ts = { .tv_sec = 0, .tv_nsec = 500'000'000};
			while (active) {
				nanosleep(&ts, nullptr);
			}

			executor->ProcessCmd(context, command);
			break;
		}
	}

	return true;
}

int Piscsi::run(const vector<char *>& args)
{
	GOOGLE_PROTOBUF_VERIFY_VERSION;

	Banner(args);

	int port = DEFAULT_PORT;
	optargs_type optargs;
	try {
		optargs = ParseArguments(args, port);
	}
	catch(const parser_exception& e) {
		cerr << "Error: " << e.what() << endl;

		return EXIT_FAILURE;
	}

	// current_log_level may have been updated by ParseArguments()
	executor->SetLogLevel(current_log_level);

	// Create a thread-safe stdout logger to process the log messages
	const auto logger = stdout_color_mt("piscsi stdout logger");

	if (!InitBus()) {
		cerr << "Error: Can't initialize bus" << endl;

		return EXIT_FAILURE;
	}

	// We need to wait to create the devices until after the bus/controller/etc objects have been created
	// TODO Try to resolve dependencies so that this work-around can be removed
	try {
		CreateInitialDevices(optargs);
	}
	catch(const parser_exception& e) {
		cerr << "Error: " << e.what() << endl;

		Cleanup();

		return EXIT_FAILURE;
	}

	if (!service.Init(&ExecuteCommand, port)) {
		return EXIT_FAILURE;
	}

	// Signal handler to detach all devices on a KILL or TERM signal
	struct sigaction termination_handler;
	termination_handler.sa_handler = TerminationHandler;
	sigemptyset(&termination_handler.sa_mask);
	termination_handler.sa_flags = 0;
	sigaction(SIGINT, &termination_handler, nullptr);
	sigaction(SIGTERM, &termination_handler, nullptr);

    // Set the affinity to a specific processor core
	FixCpu(3);

	sched_param schparam;
#ifdef USE_SEL_EVENT_ENABLE
	// Scheduling policy setting (highest priority)
	schparam.sched_priority = sched_get_priority_max(SCHED_FIFO);
	sched_setscheduler(0, SCHED_FIFO, &schparam);
#else
	cout << "Note: No PiSCSI hardware support, only client interface calls are supported" << endl;
#endif

	// Start execution
	service.SetRunning(true);

	// Main Loop
	while (service.IsRunning()) {
#ifdef USE_SEL_EVENT_ENABLE
		// SEL signal polling
		if (!bus->PollSelectEvent()) {
			// Stop on interrupt
			if (errno == EINTR) {
				break;
			}
			continue;
		}

		// Get the bus
		bus->Acquire();
#else
		bus->Acquire();
		if (!bus->GetSEL()) {
			const timespec ts = { .tv_sec = 0, .tv_nsec = 0};
			nanosleep(&ts, nullptr);
			continue;
		}
#endif

        // Wait until BSY is released as there is a possibility for the
        // initiator to assert it while setting the ID (for up to 3 seconds)
		WaitForNotBusy();

		// Stop because the bus is busy or another device responded
		if (bus->GetBSY() || !bus->GetSEL()) {
			continue;
		}

		int initiator_id = AbstractController::UNKNOWN_INITIATOR_ID;

		// The initiator and target ID
		const uint8_t id_data = bus->GetDAT();

		phase_t phase = phase_t::busfree;

		// Identify the responsible controller
		auto controller = controller_manager->IdentifyController(id_data);
		if (controller != nullptr) {
			device_logger.SetIdAndLun(controller->GetTargetId(), -1);

			initiator_id = controller->ExtractInitiatorId(id_data);

			if (initiator_id != AbstractController::UNKNOWN_INITIATOR_ID) {
				device_logger.Trace("++++ Starting processing for initiator ID " + to_string(initiator_id));
			}
			else {
				device_logger.Trace("++++ Starting processing for unknown initiator ID");
			}

			if (controller->Process(initiator_id) == phase_t::selection) {
				phase = phase_t::selection;
			}
		}

		// Return to bus monitoring if the selection phase has not started
		if (phase != phase_t::selection) {
			continue;
		}

		// Start target device
		active = true;

#if !defined(USE_SEL_EVENT_ENABLE) && defined(__linux__)
		// Scheduling policy setting (highest priority)
		schparam.sched_priority = sched_get_priority_max(SCHED_FIFO);
		sched_setscheduler(0, SCHED_FIFO, &schparam);
#endif

		// Loop until the bus is free
		while (service.IsRunning()) {
			// Target drive
			phase = controller->Process(initiator_id);

			// End when the bus is free
			if (phase == phase_t::busfree) {
				break;
			}
		}

#if !defined(USE_SEL_EVENT_ENABLE) && defined(__linux__)
		// Set the scheduling priority back to normal
		schparam.sched_priority = 0;
		sched_setscheduler(0, SCHED_OTHER, &schparam);
#endif

		// End the target travel
		active = false;
	}

	return EXIT_SUCCESS;
}

void Piscsi::WaitForNotBusy() const
{
	if (bus->GetBSY()) {
		const uint32_t now = SysTimer::GetTimerLow();

		// Wait for 3s
		while ((SysTimer::GetTimerLow() - now) < 3'000'000) {
			bus->Acquire();

			if (!bus->GetBSY()) {
				break;
			}
		}
	}
}
