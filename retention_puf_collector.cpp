#include "instruction.h"
#include "prog.h"
#include "platform.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

using std::cerr;
using std::cout;
using std::endl;
using std::map;
using std::ofstream;
using std::runtime_error;
using std::string;
using std::vector;

namespace {

constexpr int CASR = 0;
constexpr int BASR = 1;
constexpr int RASR = 2;
constexpr int CAR = 4;
constexpr int RAR = 6;
constexpr int BAR = 7;
constexpr int PATTERN_REG = 12;
constexpr int MAX_PROGRAM_INSTRUCTIONS = 2048;
constexpr uint64_t MAX_SLEEP_CYCLES = 0xFFFFFFFEULL;
constexpr uint32_t RAW_RECORD_SIZE = 128;
const char RAW_MAGIC[8] = {'R','T','P','U','F','0','0','1'};

struct Args {
  string output_dir;
  string dimm_id;
  string run_id;
  string region_id;
  string challenge_id;
  string mode;
  string temperature_source;
  string voltage_source;
  string operator_name;
  string institution;
  string board_id;
  string bitstream_file;

  int rank = 0;
  int bank_group = 0;
  int bank = 0;
  int banks_per_group = 4;
  int row_start = 0;
  int row_end = 0;
  int cache_lines_per_row = 128;
  int cache_line_bytes = 64;
  int column_stride = 8;
  int repetitions = 10;

  int pattern_byte = 0;
  int nominal_trcd_slots = 9;
  int nominal_trp_slots = 9;
  int write_spacing_slots = 7;
  int read_spacing_slots = 7;
  int write_recovery_slots = 8;
  int read_to_precharge_slots = 8;
  int final_guard_slots = 16;

  double retention_time_ms = 64.0;
  double fabric_cycle_ns = 6.0;
  double slot_ns = 1.5;
  double temperature_c = 22.0;
  double voltage_vdd_v = 1.20;

  bool write_observations_csv = false;
  bool write_raw_records_csv = true;
};

string now_utc() {
  std::time_t t = std::time(nullptr);
  std::tm tm_value;
  gmtime_r(&t, &tm_value);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_value);
  return string(buf);
}

string json_escape(const string &value) {
  std::ostringstream out;
  for (char c : value) {
    switch (c) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
              << static_cast<int>(static_cast<unsigned char>(c));
        } else {
          out << c;
        }
    }
  }
  return out.str();
}

string csv_escape(const string &value) {
  bool quote = false;
  for (char c : value) {
    if (c == ',' || c == '"' || c == '\n' || c == '\r') { quote = true; break; }
  }
  if (!quote) return value;
  string out = "\"";
  for (char c : value) out += (c == '"') ? "\"\"" : string(1, c);
  out += "\"";
  return out;
}

bool path_exists(const string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

void mkdir_p(const string &path) {
  if (path.empty() || path == ".") return;
  string current;
  if (path[0] == '/') current = "/";
  std::stringstream ss(path);
  string part;
  while (std::getline(ss, part, '/')) {
    if (part.empty()) continue;
    if (!current.empty() && current.back() != '/') current += '/';
    current += part;
    if (::mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
      throw runtime_error("mkdir failed for " + current + ": " + std::strerror(errno));
    }
  }
}

bool parse_bool(const string &value) {
  if (value == "1" || value == "true" || value == "TRUE" || value == "yes") return true;
  if (value == "0" || value == "false" || value == "FALSE" || value == "no") return false;
  throw runtime_error("Invalid boolean: " + value);
}

int parse_int(const string &name, const string &value) {
  try {
    size_t used = 0;
    int result = std::stoi(value, &used, 0);
    if (used != value.size()) throw runtime_error("");
    return result;
  } catch (...) {
    throw runtime_error("Invalid integer for " + name + ": " + value);
  }
}

double parse_double(const string &name, const string &value) {
  try {
    size_t used = 0;
    double result = std::stod(value, &used);
    if (used != value.size() || !std::isfinite(result)) throw runtime_error("");
    return result;
  } catch (...) {
    throw runtime_error("Invalid number for " + name + ": " + value);
  }
}

map<string, string> parse_cli(int argc, char **argv) {
  map<string, string> result;
  for (int i = 1; i < argc; ++i) {
    string key = argv[i];
    if (key.rfind("--", 0) != 0) throw runtime_error("Unexpected positional argument: " + key);
    if (i + 1 >= argc) throw runtime_error("Missing value after " + key);
    result[key.substr(2)] = argv[++i];
  }
  return result;
}

string require(const map<string, string> &cli, const string &key) {
  auto it = cli.find(key);
  if (it == cli.end() || it->second.empty()) throw runtime_error("Missing required --" + key);
  return it->second;
}

string optional(const map<string, string> &cli, const string &key, const string &fallback) {
  auto it = cli.find(key);
  return it == cli.end() ? fallback : it->second;
}

Args load_args(int argc, char **argv) {
  const map<string, string> cli = parse_cli(argc, argv);
  Args a;
  a.output_dir = require(cli, "output-dir");
  a.dimm_id = require(cli, "dimm-id");
  a.run_id = require(cli, "run-id");
  a.region_id = require(cli, "region-id");
  a.challenge_id = require(cli, "challenge-id");
  a.mode = require(cli, "mode");
  a.pattern_byte = parse_int("pattern", require(cli, "pattern"));
  a.rank = parse_int("rank", require(cli, "rank"));
  a.bank_group = parse_int("bank-group", require(cli, "bank-group"));
  a.bank = parse_int("bank", require(cli, "bank"));
  a.row_start = parse_int("row-start", require(cli, "row-start"));
  a.row_end = parse_int("row-end", require(cli, "row-end"));
  a.retention_time_ms = parse_double("retention-time-ms", require(cli, "retention-time-ms"));
  a.nominal_trcd_slots = parse_int("nominal-trcd-slots", require(cli, "nominal-trcd-slots"));
  a.nominal_trp_slots = parse_int("nominal-trp-slots", require(cli, "nominal-trp-slots"));
  a.temperature_c = parse_double("temperature-c", require(cli, "temperature-c"));
  a.voltage_vdd_v = parse_double("voltage-vdd-v", require(cli, "voltage-vdd-v"));

  a.banks_per_group = parse_int("banks-per-group", optional(cli, "banks-per-group", "4"));
  a.cache_lines_per_row = parse_int("cache-lines-per-row", optional(cli, "cache-lines-per-row", "128"));
  a.cache_line_bytes = parse_int("cache-line-bytes", optional(cli, "cache-line-bytes", "64"));
  a.column_stride = parse_int("column-stride", optional(cli, "column-stride", "8"));
  a.repetitions = parse_int("repetitions", optional(cli, "repetitions", "10"));
  a.write_spacing_slots = parse_int("write-spacing-slots", optional(cli, "write-spacing-slots", "7"));
  a.read_spacing_slots = parse_int("read-spacing-slots", optional(cli, "read-spacing-slots", "7"));
  a.write_recovery_slots = parse_int("write-recovery-slots", optional(cli, "write-recovery-slots", "8"));
  a.read_to_precharge_slots = parse_int("read-to-precharge-slots", optional(cli, "read-to-precharge-slots", "8"));
  a.final_guard_slots = parse_int("final-guard-slots", optional(cli, "final-guard-slots", "16"));
  a.fabric_cycle_ns = parse_double("fabric-cycle-ns", optional(cli, "fabric-cycle-ns", "6.0"));
  a.slot_ns = parse_double("slot-ns", optional(cli, "slot-ns", "1.5"));
  a.write_observations_csv = parse_bool(optional(cli, "write-observations-csv", "false"));
  a.write_raw_records_csv = parse_bool(optional(cli, "write-raw-records-csv", "true"));

  a.temperature_source = optional(cli, "temperature-source", "manual");
  a.voltage_source = optional(cli, "voltage-source", "manual");
  a.operator_name = optional(cli, "operator", "");
  a.institution = optional(cli, "institution", "");
  a.board_id = optional(cli, "board-id", "ALVEO_U200_01");
  a.bitstream_file = optional(cli, "bitstream-file", "unknown");

  if (a.pattern_byte < 0 || a.pattern_byte > 255) throw runtime_error("pattern must be 0..255");
  if (a.rank < 0 || a.rank > 3) throw runtime_error("rank must be non-negative and supported by the bitstream");
  if (a.bank_group < 0 || a.bank < 0 || a.banks_per_group <= 0) throw runtime_error("invalid bank coordinates");
  if (a.row_start < 0 || a.row_end < a.row_start) throw runtime_error("row range must be inclusive and non-empty");
  if (a.cache_lines_per_row <= 0 || a.cache_line_bytes != 64 || a.column_stride <= 0)
    throw runtime_error("this U200 collector requires positive cache_lines_per_row, cache_line_bytes=64, positive column_stride");
  if (a.repetitions <= 0) throw runtime_error("repetitions must be positive");
  if (a.retention_time_ms < 0.0) throw runtime_error("retention-time-ms cannot be negative");
  if (a.fabric_cycle_ns <= 0.0 || a.slot_ns <= 0.0) throw runtime_error("cycle metadata must be positive");
  const int timings[] = {a.nominal_trcd_slots, a.nominal_trp_slots, a.write_spacing_slots,
                         a.read_spacing_slots, a.write_recovery_slots,
                         a.read_to_precharge_slots, a.final_guard_slots};
  for (int v : timings) if (v < 0) throw runtime_error("timing slot counts cannot be negative");
  const int flat_bank = a.bank_group * a.banks_per_group + a.bank;
  if (flat_bank < 0 || flat_bank > 15)
    throw runtime_error("encoded flat bank is outside 0..15; verify bank geometry and U200 bitstream");
  return a;
}

class DdrPacker {
 public:
  DdrPacker(Program &program, int rank) : program_(program), rank_(rank) {}
  void emit(Mininst command) {
    slots_.push_back(command);
    if (slots_.size() == 4) flush_full();
  }
  void nops(int count) { for (int i = 0; i < count; ++i) emit(SMC_NOP(rank_)); }
  void flush() {
    while (!slots_.empty() && slots_.size() < 4) slots_.push_back(SMC_NOP(rank_));
    if (slots_.size() == 4) flush_full();
  }
 private:
  void flush_full() {
    program_.add_inst(__pack_mininsts(slots_[0], slots_[1], slots_[2], slots_[3]));
    slots_.clear();
  }
  Program &program_;
  int rank_;
  vector<Mininst> slots_;
};

uint32_t repeated_pattern32(int pattern_byte) {
  const uint32_t b = static_cast<uint32_t>(pattern_byte & 0xff);
  return b | (b << 8) | (b << 16) | (b << 24);
}

uint64_t retention_sleep_cycles(const Args &a) {
  if (a.retention_time_ms <= 0.0) return 0;
  const long double requested_ns = static_cast<long double>(a.retention_time_ms) * 1000000.0L;
  uint64_t cycles = static_cast<uint64_t>(std::llround(requested_ns / a.fabric_cycle_ns));
  if (cycles > 0 && cycles < 3) cycles = 3;  // SMC_SLEEP requires >2 cycles.
  return cycles;
}

double scheduled_retention_ms(const Args &a) {
  return static_cast<double>(retention_sleep_cycles(a)) * a.fabric_cycle_ns / 1000000.0;
}

int sleep_instruction_count(const Args &a) {
  const uint64_t cycles = retention_sleep_cycles(a);
  if (cycles == 0) return 0;
  return static_cast<int>((cycles + MAX_SLEEP_CYCLES - 1) / MAX_SLEEP_CYCLES);
}

void append_retention_sleep(Program &program, const Args &a) {
  uint64_t remaining = retention_sleep_cycles(a);
  while (remaining > 0) {
    const uint64_t chunk = std::min<uint64_t>(remaining, MAX_SLEEP_CYCLES);
    program.add_inst(SMC_SLEEP(static_cast<uint32_t>(chunk)));
    remaining -= chunk;
  }
}

int estimate_program_instructions(const Args &a) {
  const int setup = 1 + 1 + 1 + 1 + 1 + 2 + 1 + 16;
  const int ddr_slots =
      1 + a.nominal_trp_slots + 1 + a.nominal_trcd_slots +
      a.cache_lines_per_row + std::max(0, a.cache_lines_per_row - 1) * a.write_spacing_slots +
      a.write_recovery_slots + 1 + a.nominal_trp_slots +
      1 + a.nominal_trcd_slots +
      a.cache_lines_per_row + std::max(0, a.cache_lines_per_row - 1) * a.read_spacing_slots +
      a.read_to_precharge_slots + 1 + a.final_guard_slots;
  return setup + (ddr_slots + 3) / 4 + sleep_instruction_count(a) + 1;
}

Program build_row_program(const Args &a, int row) {
  const int estimated = estimate_program_instructions(a);
  if (estimated >= MAX_PROGRAM_INSTRUCTIONS) {
    std::ostringstream msg;
    msg << "Program would require about " << estimated
        << " instructions, exceeding the 2048-instruction frontend. "
        << "Reduce cache_lines_per_row/spacing or use a shorter retention delay.";
    throw runtime_error(msg.str());
  }

  const int flat_bank = a.bank_group * a.banks_per_group + a.bank;
  Program program;
  program.add_inst(SMC_LI(a.column_stride, CASR));
  program.add_inst(SMC_LI(1, BASR));
  program.add_inst(SMC_LI(1, RASR));
  program.add_inst(SMC_LI(flat_bank, BAR));
  program.add_inst(SMC_LI(row, RAR));
  program.add_inst(SMC_LI(0, CAR));

  const uint32_t pattern32 = repeated_pattern32(a.pattern_byte);
  program.add_inst(SMC_LI(pattern32, PATTERN_REG));
  for (int i = 0; i < 16; ++i) program.add_inst(SMC_LDWD(PATTERN_REG, i));

  DdrPacker init(program, a.rank);
  init.emit(SMC_PRE(BAR, 0, 0, a.rank));
  init.nops(a.nominal_trp_slots);
  init.emit(SMC_ACT(BAR, 0, RAR, 0, a.rank));
  init.nops(a.nominal_trcd_slots);
  for (int cl = 0; cl < a.cache_lines_per_row; ++cl) {
    init.emit(SMC_WRITE(BAR, 0, CAR, 1, a.rank, 0));
    if (cl + 1 < a.cache_lines_per_row) init.nops(a.write_spacing_slots);
  }
  init.nops(a.write_recovery_slots);
  init.emit(SMC_PRE(BAR, 0, 0, a.rank));
  init.nops(a.nominal_trp_slots);
  init.flush();

  // The configured retention time is an additional refresh-off dwell after
  // the row has been safely precharged. Auto-refresh is disabled by the host
  // for the entire row program.
  append_retention_sleep(program, a);

  program.add_inst(SMC_LI(0, CAR));
  DdrPacker reads(program, a.rank);
  reads.emit(SMC_ACT(BAR, 0, RAR, 0, a.rank));
  reads.nops(a.nominal_trcd_slots);
  for (int cl = 0; cl < a.cache_lines_per_row; ++cl) {
    reads.emit(SMC_READ(BAR, 0, CAR, 1, a.rank, 0));
    if (cl + 1 < a.cache_lines_per_row) reads.nops(a.read_spacing_slots);
  }
  reads.nops(a.read_to_precharge_slots);
  reads.emit(SMC_PRE(BAR, 0, 0, a.rank));
  reads.nops(a.final_guard_slots);
  reads.flush();
  program.add_inst(SMC_END());
  return program;
}

string rep_name(int repetition) {
  std::ostringstream out;
  out << "REP_" << std::setw(2) << std::setfill('0') << repetition;
  return out.str();
}

string pattern_hex(const Args &a) {
  std::ostringstream out;
  out << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << a.pattern_byte;
  return out.str();
}

void write_u32_le(ofstream &out, uint32_t v) {
  char b[4];
  for (int i = 0; i < 4; ++i) b[i] = static_cast<char>((v >> (8 * i)) & 0xff);
  out.write(b, 4);
}
void write_i32_le(ofstream &out, int32_t v) { write_u32_le(out, static_cast<uint32_t>(v)); }
void write_u64_le(ofstream &out, uint64_t v) {
  char b[8];
  for (int i = 0; i < 8; ++i) b[i] = static_cast<char>((v >> (8 * i)) & 0xff);
  out.write(b, 8);
}

string raw_header_json(const Args &a, int repetition) {
  const int flat_bank = a.bank_group * a.banks_per_group + a.bank;
  std::ostringstream out;
  out << "{";
  out << "\"format\":\"DRAM_BENDER_RETENTION_PUF_RAW\",";
  out << "\"version\":1,";
  out << "\"record_size_bytes\":" << RAW_RECORD_SIZE << ',';
  out << "\"dimm_id\":\"" << json_escape(a.dimm_id) << "\",";
  out << "\"run_id\":\"" << json_escape(a.run_id) << "\",";
  out << "\"region_id\":\"" << json_escape(a.region_id) << "\",";
  out << "\"challenge_id\":\"" << json_escape(a.challenge_id) << "\",";
  out << "\"pattern_hex\":\"" << pattern_hex(a) << "\",";
  out << "\"mode\":\"" << json_escape(a.mode) << "\",";
  out << "\"repetition\":" << repetition << ',';
  out << "\"retention_time_ms_requested\":" << std::setprecision(12) << a.retention_time_ms << ',';
  out << "\"retention_time_ms_scheduled\":" << std::setprecision(12) << scheduled_retention_ms(a) << ',';
  out << "\"retention_sleep_cycles\":" << retention_sleep_cycles(a) << ',';
  out << "\"fabric_cycle_ns\":" << a.fabric_cycle_ns << ',';
  out << "\"temperature_c\":" << a.temperature_c << ',';
  out << "\"voltage_vdd_v\":" << a.voltage_vdd_v << ',';
  out << "\"rank\":" << a.rank << ',';
  out << "\"bank_group\":" << a.bank_group << ',';
  out << "\"bank\":" << a.bank << ',';
  out << "\"flat_bank\":" << flat_bank << ',';
  out << "\"row_start_inclusive\":" << a.row_start << ',';
  out << "\"row_end_inclusive\":" << a.row_end << ',';
  out << "\"cache_lines_per_row\":" << a.cache_lines_per_row << ',';
  out << "\"cache_line_bytes\":" << a.cache_line_bytes << ',';
  out << "\"column_stride\":" << a.column_stride << ',';
  out << "\"record_layout\":\"<Q I i i i i I I I Q I B 3x Q 64s\"";
  out << "}";
  return out.str();
}

void write_raw_header(ofstream &out, const Args &a, int repetition) {
  const string header = raw_header_json(a, repetition);
  out.write(RAW_MAGIC, 8);
  write_u32_le(out, static_cast<uint32_t>(header.size()));
  out.write(header.data(), static_cast<std::streamsize>(header.size()));
}

void write_raw_record(ofstream &out, uint64_t record_index, int repetition,
                      const Args &a, int row, int cache_line, uint64_t reads_offset,
                      const uint8_t *payload) {
  const int flat_bank = a.bank_group * a.banks_per_group + a.bank;
  const uint32_t column = static_cast<uint32_t>(cache_line * a.column_stride);
  write_u64_le(out, record_index);
  write_u32_le(out, static_cast<uint32_t>(repetition));
  write_i32_le(out, a.rank);
  write_i32_le(out, a.bank_group);
  write_i32_le(out, a.bank);
  write_i32_le(out, flat_bank);
  write_u32_le(out, static_cast<uint32_t>(row));
  write_u32_le(out, static_cast<uint32_t>(cache_line));
  write_u32_le(out, column);
  write_u64_le(out, reads_offset);
  write_u32_le(out, 64);
  const char p = static_cast<char>(a.pattern_byte & 0xff);
  out.write(&p, 1);
  const char reserved[3] = {0,0,0};
  out.write(reserved, 3);
  write_u64_le(out, retention_sleep_cycles(a));
  out.write(reinterpret_cast<const char *>(payload), 64);
}

void write_trial_json(const Args &a, const string &rep_dir, int repetition,
                      const string &status, int rows_completed, uint64_t bytes_received,
                      int current_row, const string &error_message,
                      const string &started_at, const string &finished_at) {
  const string temp_path = rep_dir + "/trial.json.tmp";
  const string final_path = rep_dir + "/trial.json";
  ofstream out(temp_path.c_str(), std::ios::trunc);
  if (!out) return;
  const int row_count = a.row_end - a.row_start + 1;
  const uint64_t expected_bytes = static_cast<uint64_t>(row_count) * a.cache_lines_per_row * a.cache_line_bytes;
  const int flat_bank = a.bank_group * a.banks_per_group + a.bank;
  out << "{\n";
  out << "  \"status\": \"" << json_escape(status) << "\",\n";
  out << "  \"started_at_utc\": \"" << json_escape(started_at) << "\",\n";
  out << "  \"finished_at_utc\": \"" << json_escape(finished_at) << "\",\n";
  out << "  \"dimm_id\": \"" << json_escape(a.dimm_id) << "\",\n";
  out << "  \"run_id\": \"" << json_escape(a.run_id) << "\",\n";
  out << "  \"region_id\": \"" << json_escape(a.region_id) << "\",\n";
  out << "  \"challenge_id\": \"" << json_escape(a.challenge_id) << "\",\n";
  out << "  \"pattern_hex\": \"" << pattern_hex(a) << "\",\n";
  out << "  \"mode\": \"" << json_escape(a.mode) << "\",\n";
  out << "  \"repetition\": " << repetition << ",\n";
  out << "  \"retention_time_ms_requested\": " << std::setprecision(12) << a.retention_time_ms << ",\n";
  out << "  \"retention_time_ms_scheduled\": " << scheduled_retention_ms(a) << ",\n";
  out << "  \"retention_sleep_cycles\": " << retention_sleep_cycles(a) << ",\n";
  out << "  \"fabric_cycle_ns\": " << a.fabric_cycle_ns << ",\n";
  out << "  \"refresh_policy\": \"auto-refresh disabled while each row write/wait/read program executes; re-enabled between rows\",\n";
  out << "  \"temperature_c\": " << a.temperature_c << ",\n";
  out << "  \"temperature_source\": \"" << json_escape(a.temperature_source) << "\",\n";
  out << "  \"voltage_vdd_v\": " << a.voltage_vdd_v << ",\n";
  out << "  \"voltage_source\": \"" << json_escape(a.voltage_source) << "\",\n";
  out << "  \"rank\": " << a.rank << ",\n";
  out << "  \"bank_group\": " << a.bank_group << ",\n";
  out << "  \"bank\": " << a.bank << ",\n";
  out << "  \"flat_bank\": " << flat_bank << ",\n";
  out << "  \"row_start_inclusive\": " << a.row_start << ",\n";
  out << "  \"row_end_inclusive\": " << a.row_end << ",\n";
  out << "  \"current_row\": " << current_row << ",\n";
  out << "  \"rows_completed\": " << rows_completed << ",\n";
  out << "  \"cache_lines_per_row\": " << a.cache_lines_per_row << ",\n";
  out << "  \"cache_line_bytes\": " << a.cache_line_bytes << ",\n";
  out << "  \"column_stride\": " << a.column_stride << ",\n";
  out << "  \"nominal_trcd_slots\": " << a.nominal_trcd_slots << ",\n";
  out << "  \"nominal_trp_slots\": " << a.nominal_trp_slots << ",\n";
  out << "  \"slot_ns_metadata\": " << a.slot_ns << ",\n";
  out << "  \"expected_bytes\": " << expected_bytes << ",\n";
  out << "  \"bytes_received\": " << bytes_received << ",\n";
  out << "  \"raw_file\": \"reads.bin\",\n";
  out << "  \"addressed_raw_file\": \"raw_records.bin\",\n";
  out << "  \"raw_records_csv\": " << (a.write_raw_records_csv ? "\"raw_records.csv\"" : "null") << ",\n";
  out << "  \"observations_file\": " << (a.write_observations_csv ? "\"observations.csv\"" : "null") << ",\n";
  out << "  \"flips_file\": \"flips.csv\",\n";
  out << "  \"read_index_file\": \"read_index.csv\",\n";
  out << "  \"board_id\": \"" << json_escape(a.board_id) << "\",\n";
  out << "  \"bitstream_file\": \"" << json_escape(a.bitstream_file) << "\",\n";
  out << "  \"operator\": \"" << json_escape(a.operator_name) << "\",\n";
  out << "  \"institution\": \"" << json_escape(a.institution) << "\",\n";
  out << "  \"error\": \"" << json_escape(error_message) << "\"\n";
  out << "}\n";
  out.close();
  std::rename(temp_path.c_str(), final_path.c_str());
}

string common_csv_header() {
  return "dimm_id,run_id,region_id,challenge_id,pattern_hex,mode,retention_time_ms_requested,"
         "retention_time_ms_scheduled,retention_sleep_cycles,repetition,temperature_c,temperature_source,"
         "voltage_vdd_v,voltage_source,rank,bank_group,bank,flat_bank,row,cache_line,column,"
         "byte_in_cache_line,bit_in_byte,bit_index_in_cache_line,bit_index_in_row,raw_byte_offset,"
         "expected_bit,observed_bit,did_flip,flip_direction\n";
}

void write_bit_row(ofstream &out, const Args &a, int repetition, int row, int cache_line,
                   int byte_in_cache_line, int bit_in_byte, uint64_t raw_byte_offset,
                   int expected_bit, int observed_bit) {
  const bool flipped = expected_bit != observed_bit;
  const int flat_bank = a.bank_group * a.banks_per_group + a.bank;
  const int column = cache_line * a.column_stride;
  const int bit_index_in_cache_line = byte_in_cache_line * 8 + bit_in_byte;
  const uint64_t row_byte_offset = static_cast<uint64_t>(cache_line) * a.cache_line_bytes + byte_in_cache_line;
  const uint64_t bit_index_in_row = row_byte_offset * 8 + bit_in_byte;
  string direction;
  if (flipped) direction = expected_bit == 0 ? "0_to_1" : "1_to_0";
  out << csv_escape(a.dimm_id) << ',' << csv_escape(a.run_id) << ',' << csv_escape(a.region_id) << ','
      << csv_escape(a.challenge_id) << ',' << pattern_hex(a) << ',' << csv_escape(a.mode) << ','
      << std::setprecision(12) << a.retention_time_ms << ',' << scheduled_retention_ms(a) << ','
      << retention_sleep_cycles(a) << ',' << repetition << ','
      << a.temperature_c << ',' << csv_escape(a.temperature_source) << ','
      << a.voltage_vdd_v << ',' << csv_escape(a.voltage_source) << ','
      << a.rank << ',' << a.bank_group << ',' << a.bank << ',' << flat_bank << ','
      << row << ',' << cache_line << ',' << column << ',' << byte_in_cache_line << ',' << bit_in_byte << ','
      << bit_index_in_cache_line << ',' << bit_index_in_row << ',' << raw_byte_offset << ','
      << expected_bit << ',' << observed_bit << ',' << (flipped ? 1 : 0) << ',' << direction << '\n';
}

class RefreshOffGuard {
 public:
  explicit RefreshOffGuard(SoftMCPlatform &platform) : platform_(platform), active_(true) {
    platform_.set_aref(false);
  }
  ~RefreshOffGuard() {
    if (active_) platform_.set_aref(true);
  }
  void restore() {
    if (active_) {
      platform_.set_aref(true);
      active_ = false;
    }
  }
 private:
  SoftMCPlatform &platform_;
  bool active_;
};

void collect_repetition(SoftMCPlatform &platform, const Args &a, int repetition) {
  const string rep_dir = a.output_dir + "/" + rep_name(repetition);
  mkdir_p(rep_dir);
  const string complete_marker = rep_dir + "/.complete";
  if (path_exists(complete_marker)) {
    cout << "Skipping completed " << rep_name(repetition) << endl;
    return;
  }

  const string started_at = now_utc();
  uint64_t bytes_received = 0;
  uint64_t record_index = 0;
  int rows_completed = 0;
  int current_row = a.row_start;
  write_trial_json(a, rep_dir, repetition, "running", rows_completed, bytes_received,
                   current_row, "", started_at, "");

  ofstream raw((rep_dir + "/reads.bin").c_str(), std::ios::binary | std::ios::trunc);
  ofstream addressed((rep_dir + "/raw_records.bin").c_str(), std::ios::binary | std::ios::trunc);
  ofstream index((rep_dir + "/read_index.csv").c_str(), std::ios::trunc);
  ofstream flips((rep_dir + "/flips.csv").c_str(), std::ios::trunc);
  ofstream raw_csv;
  ofstream observations;
  if (a.write_raw_records_csv) raw_csv.open((rep_dir + "/raw_records.csv").c_str(), std::ios::trunc);
  if (a.write_observations_csv) observations.open((rep_dir + "/observations.csv").c_str(), std::ios::trunc);
  if (!raw || !addressed || !index || !flips || (a.write_raw_records_csv && !raw_csv) ||
      (a.write_observations_csv && !observations)) {
    throw runtime_error("Unable to open one or more output files in " + rep_dir);
  }

  write_raw_header(addressed, a, repetition);
  index << "dimm_id,run_id,region_id,challenge_id,pattern_hex,mode,retention_time_ms_requested,"
           "retention_time_ms_scheduled,retention_sleep_cycles,repetition,temperature_c,voltage_vdd_v,"
           "rank,bank_group,bank,flat_bank,row,cache_line,column,raw_byte_offset,length_bytes\n";
  if (a.write_raw_records_csv) {
    raw_csv << "dimm_id,run_id,region_id,challenge_id,pattern_hex,mode,retention_time_ms_requested,"
               "retention_time_ms_scheduled,retention_sleep_cycles,repetition,temperature_c,voltage_vdd_v,"
               "rank,bank_group,bank,flat_bank,row,cache_line,column,raw_byte_offset,data_hex\n";
  }
  flips << common_csv_header();
  if (a.write_observations_csv) observations << common_csv_header();

  const size_t row_bytes = static_cast<size_t>(a.cache_lines_per_row) * a.cache_line_bytes;
  if (row_bytes == 0 || row_bytes % 4 != 0) throw runtime_error("row read size must be a positive multiple of four");
  vector<uint8_t> buffer(row_bytes, 0);
  const uint8_t expected_byte = static_cast<uint8_t>(a.pattern_byte);

  try {
    for (int row = a.row_start; row <= a.row_end; ++row) {
      current_row = row;
      Program program = build_row_program(a, row);
      int received = 0;
      {
        RefreshOffGuard refresh_guard(platform);
        platform.execute(program);
        received = platform.receiveData(buffer.data(), static_cast<int>(row_bytes));
        refresh_guard.restore();
      }
      if (received != static_cast<int>(row_bytes)) {
        std::ostringstream msg;
        msg << "receiveData returned " << received << " bytes; expected " << row_bytes;
        throw runtime_error(msg.str());
      }

      const uint64_t row_base_offset = bytes_received;
      raw.write(reinterpret_cast<const char *>(buffer.data()), static_cast<std::streamsize>(row_bytes));
      if (!raw) throw runtime_error("failed while writing reads.bin");
      const int flat_bank = a.bank_group * a.banks_per_group + a.bank;

      for (int cl = 0; cl < a.cache_lines_per_row; ++cl) {
        const size_t row_offset = static_cast<size_t>(cl) * a.cache_line_bytes;
        const uint64_t cache_line_offset = row_base_offset + row_offset;
        const int column = cl * a.column_stride;
        index << csv_escape(a.dimm_id) << ',' << csv_escape(a.run_id) << ',' << csv_escape(a.region_id) << ','
              << csv_escape(a.challenge_id) << ',' << pattern_hex(a) << ',' << csv_escape(a.mode) << ','
              << std::setprecision(12) << a.retention_time_ms << ',' << scheduled_retention_ms(a) << ','
              << retention_sleep_cycles(a) << ',' << repetition << ',' << a.temperature_c << ',' << a.voltage_vdd_v << ','
              << a.rank << ',' << a.bank_group << ',' << a.bank << ',' << flat_bank << ','
              << row << ',' << cl << ',' << column << ',' << cache_line_offset << ',' << a.cache_line_bytes << '\n';

        write_raw_record(addressed, record_index++, repetition, a, row, cl, cache_line_offset, buffer.data() + row_offset);

        if (a.write_raw_records_csv) {
          raw_csv << csv_escape(a.dimm_id) << ',' << csv_escape(a.run_id) << ',' << csv_escape(a.region_id) << ','
                  << csv_escape(a.challenge_id) << ',' << pattern_hex(a) << ',' << csv_escape(a.mode) << ','
                  << std::setprecision(12) << a.retention_time_ms << ',' << scheduled_retention_ms(a) << ','
                  << retention_sleep_cycles(a) << ',' << repetition << ',' << a.temperature_c << ',' << a.voltage_vdd_v << ','
                  << a.rank << ',' << a.bank_group << ',' << a.bank << ',' << flat_bank << ','
                  << row << ',' << cl << ',' << column << ',' << cache_line_offset << ',';
          raw_csv << std::hex << std::setfill('0');
          for (int i = 0; i < a.cache_line_bytes; ++i)
            raw_csv << std::setw(2) << static_cast<unsigned int>(buffer[row_offset + i]);
          raw_csv << std::dec << '\n';
        }

        for (int byte_index = 0; byte_index < a.cache_line_bytes; ++byte_index) {
          const size_t absolute_row_offset = row_offset + byte_index;
          const uint8_t observed_byte = buffer[absolute_row_offset];
          const uint64_t raw_byte_offset = row_base_offset + absolute_row_offset;
          for (int bit = 0; bit < 8; ++bit) {
            const int expected_bit = (expected_byte >> bit) & 1;
            const int observed_bit = (observed_byte >> bit) & 1;
            if (a.write_observations_csv)
              write_bit_row(observations, a, repetition, row, cl, byte_index, bit,
                            raw_byte_offset, expected_bit, observed_bit);
            if (expected_bit != observed_bit)
              write_bit_row(flips, a, repetition, row, cl, byte_index, bit,
                            raw_byte_offset, expected_bit, observed_bit);
          }
        }
      }

      bytes_received += row_bytes;
      ++rows_completed;
      raw.flush(); addressed.flush(); index.flush(); flips.flush();
      if (a.write_raw_records_csv) raw_csv.flush();
      if (a.write_observations_csv) observations.flush();
      write_trial_json(a, rep_dir, repetition, "running", rows_completed, bytes_received,
                       current_row, "", started_at, "");
      cout << rep_name(repetition) << " row " << row << " complete ("
           << rows_completed << "/" << (a.row_end - a.row_start + 1) << ")" << endl;
    }
  } catch (const std::exception &e) {
    platform.set_aref(true);
    write_trial_json(a, rep_dir, repetition, "failed", rows_completed, bytes_received,
                     current_row, e.what(), started_at, now_utc());
    throw;
  }

  raw.close(); addressed.close(); index.close(); flips.close();
  if (a.write_raw_records_csv) raw_csv.close();
  if (a.write_observations_csv) observations.close();
  write_trial_json(a, rep_dir, repetition, "complete", rows_completed, bytes_received,
                   current_row, "", started_at, now_utc());
  ofstream marker(complete_marker.c_str(), std::ios::trunc);
  marker << now_utc() << '\n';
}

void print_usage() {
  cerr << "retention_puf_collector: collect one region/challenge/retention point\n"
       << "Arguments are normally supplied by run_experiment.py. See README.md.\n";
}

}  // namespace

int main(int argc, char **argv) {
  if (argc == 1) { print_usage(); return 2; }
  try {
    const Args args = load_args(argc, argv);
    mkdir_p(args.output_dir);
    const int flat_bank = args.bank_group * args.banks_per_group + args.bank;
    cout << "Starting DIMM=" << args.dimm_id
         << " region=" << args.region_id
         << " challenge=" << args.challenge_id
         << " mode=" << args.mode
         << " rank=" << args.rank
         << " BG=" << args.bank_group
         << " bank=" << args.bank
         << " flat_bank=" << flat_bank
         << " rows=" << args.row_start << "-" << args.row_end
         << " retention_ms=" << args.retention_time_ms
         << " scheduled_ms=" << scheduled_retention_ms(args)
         << " temp_C=" << args.temperature_c
         << " VDD_V=" << args.voltage_vdd_v << endl;

    SoftMCPlatform platform;
    const int err = platform.init();
    if (err != SOFTMC_SUCCESS)
      throw runtime_error("Could not initialize SoftMCPlatform; error code " + std::to_string(err));
    platform.reset_fpga();
    platform.set_aref(true);

    for (int repetition = 0; repetition < args.repetitions; ++repetition)
      collect_repetition(platform, args, repetition);

    platform.set_aref(true);
    cout << "Retention point complete." << endl;
    return 0;
  } catch (const std::exception &e) {
    cerr << "ERROR: " << e.what() << endl;
    return 1;
  }
}
