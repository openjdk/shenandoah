//
// Created by Young, Christian on 6/28/21.
//

#include "precompiled.hpp"
#include "jvm.h"
#include "logging/log.hpp"
#include "logging/logConfiguration.hpp"
#include "memory/allocation.inline.hpp"
#include "runtime/arguments.hpp"
#include "runtime/os.inline.hpp"
#include "runtime/perfData.inline.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/defaultStream.hpp"

#include "shenandoahLogFileOutput.hpp"
#include "logging/logFileOutput.hpp"

const char* const ShenandoahLogFileOutput::Prefix = "file=";
const char* const ShenandoahLogFileOutput::FileOpenMode = "a+";
const char* const ShenandoahLogFileOutput::PidFilenamePlaceholder = "%p";
const char* const ShenandoahLogFileOutput::TimestampFilenamePlaceholder = "%t";
const char* const ShenandoahLogFileOutput::TimestampFormat = "%Y-%m-%d_%H-%M-%S";
char        ShenandoahLogFileOutput::_pid_str[PidBufferSize];
char        ShenandoahLogFileOutput::_vm_start_time_str[StartTimeBufferSize];

// Functions/macro from LogFileStreamOutput.cpp
class FileLocker : public StackObj {
private:
    FILE *_file;

public:
    FileLocker(FILE *file) : _file(file) {
      os::flockfile(_file);
    }

    ~FileLocker() {
      os::funlockfile(_file);
    }
};

#define WRITE_LOG_WITH_RESULT_CHECK(op, total)                \
{                                                             \
  int result = op;                                            \
  if (result < 0) {                                           \
    if (!_write_error_is_shown) {                             \
      jio_fprintf(defaultStream::error_stream(),              \
                  "Could not write log: %s\n", name());       \
      jio_fprintf(_stream, "\nERROR: Could not write log\n"); \
      _write_error_is_shown = true;                           \
      return -1;                                              \
    }                                                         \
  }                                                           \
  total += result;                                            \
}

// Function from LogFileOutput.cpp
static bool file_exists(const char* filename) {
  struct stat dummy_stat;
  return os::stat(filename, &dummy_stat) == 0;
}

ShenandoahLogFileOutput::ShenandoahLogFileOutput(const char* name)
        : LogFileOutput(name), _name(os::strdup_check_oom(name, mtLogging)),
          _file_name(NULL), _snapshot_count(0), _current_size(0) {
  assert(strstr(name, Prefix) == name, "invalid output name '%s': missing prefix: %s", name, Prefix);
  _file_name = make_file_name(name, _pid_str, _vm_start_time_str);
}

ShenandoahLogFileOutput::~ShenandoahLogFileOutput() {
  if (_stream != NULL) {
    if (fclose(_stream) != 0) {
      jio_fprintf(defaultStream::error_stream(), "Could not close log file '%s' (%s).\n",
                  _file_name, os::strerror(errno));
    }
  }
  os::free(_file_name);
  os::free(const_cast<char*>(_name));
}

bool ShenandoahLogFileOutput::flush() {
  bool result = true;
  if (fflush(_stream) != 0) {
    if (!_write_error_is_shown) {
      jio_fprintf(defaultStream::error_stream(),
                  "Could not flush log: %s (%s (%d))\n", name(), os::strerror(errno), errno);
      jio_fprintf(_stream, "\nERROR: Could not flush log (%d)\n", errno);
      _write_error_is_shown = true;
    }
    result = false;
  }
  return result;
}

bool ShenandoahLogFileOutput::initialize(const char* options, outputStream* errstream) {
  _stream = os::fopen(_file_name, ShenandoahLogFileOutput::FileOpenMode);
  if (_stream == NULL) {
    errstream->print_cr("Error opening log file '%s': %s",
                        _file_name, os::strerror(errno));
    return false;
  }
  return true;
}

bool ShenandoahLogFileOutput::initialize() {
  return initialize(NULL, tty);
}

int ShenandoahLogFileOutput::write(const LogDecorations& decorations, const char* msg) {
  if (_stream == NULL) {
    // An error has occurred with this output, avoid writing to it.
    return 0;
  }

  int written = LogFileStreamOutput::write(decorations, msg);
  if (written > 0) {
    _current_size += written;
  }

  return written;
}

int ShenandoahLogFileOutput::write(LogMessageBuffer::Iterator msg_iterator) {
  if (_stream == NULL) {
    // An error has occurred with this output, avoid writing to it.
    return 0;
  }

  int written = LogFileStreamOutput::write(msg_iterator);
  if (written > 0) {
    _current_size += written;
  }

  return written;
}

int ShenandoahLogFileOutput::write_snapshot(PerfLongVariable** regions,
                                            PerfLongVariable* ts,
                                            PerfLongVariable* status,
                                            size_t num_regions,
                                            size_t rs) {
  int written = 0;
  FileLocker flocker(_stream);
  WRITE_LOG_WITH_RESULT_CHECK(jio_fprintf(_stream, "S%d:%lli;%lli;%u;%u\n",
                                          _snapshot_count++,
                                          ts->get_value(),
                                          status->get_value(),
                                          num_regions,
                                          rs),written);
  if (num_regions > 0) {
    WRITE_LOG_WITH_RESULT_CHECK(jio_fprintf(_stream, "%lli", regions[0]->get_value()), written);
  }
  for (uint i = 1; i < num_regions; ++i) {
    WRITE_LOG_WITH_RESULT_CHECK(jio_fprintf(_stream, " %lli", regions[i]->get_value()), written);
  }
  jio_fprintf(_stream, "\n");
  return flush() ? written : -1;
}

void ShenandoahLogFileOutput::set_file_name_parameters(jlong vm_start_time) {
  int res = jio_snprintf(_pid_str, sizeof(_pid_str), "%d", os::current_process_id());
  assert(res > 0, "PID buffer too small");

  struct tm local_time;
  time_t utc_time = vm_start_time / 1000;
  os::localtime_pd(&utc_time, &local_time);
  res = (int)strftime(_vm_start_time_str, sizeof(_vm_start_time_str), TimestampFormat, &local_time);
  assert(res > 0, "VM start time buffer too small.");
}

char* ShenandoahLogFileOutput::make_file_name(const char* file_name,
                                    const char* pid_string,
                                    const char* timestamp_string) {
  char* result = NULL;

  // Lets start finding out if we have any %d and/or %t in the name.
  // We will only replace the first occurrence of any placeholder
  const char* pid = strstr(file_name, PidFilenamePlaceholder);
  const char* timestamp = strstr(file_name, TimestampFilenamePlaceholder);

  if (pid == NULL && timestamp == NULL) {
    // We found no place-holders, return the simple filename
    return os::strdup_check_oom(file_name, mtLogging);
  }

  // At least one of the place-holders were found in the file_name
  const char* first = "";
  size_t first_pos = SIZE_MAX;
  size_t first_replace_len = 0;

  const char* second = "";
  size_t second_pos = SIZE_MAX;
  size_t second_replace_len = 0;

  // If we found a %p, then setup our variables accordingly
  if (pid != NULL) {
    if (timestamp == NULL || pid < timestamp) {
      first = pid_string;
      first_pos = pid - file_name;
      first_replace_len = strlen(PidFilenamePlaceholder);
    } else {
      second = pid_string;
      second_pos = pid - file_name;
      second_replace_len = strlen(PidFilenamePlaceholder);
    }
  }

  if (timestamp != NULL) {
    if (pid == NULL || timestamp < pid) {
      first = timestamp_string;
      first_pos = timestamp - file_name;
      first_replace_len = strlen(TimestampFilenamePlaceholder);
    } else {
      second = timestamp_string;
      second_pos = timestamp - file_name;
      second_replace_len = strlen(TimestampFilenamePlaceholder);
    }
  }

  size_t first_len = strlen(first);
  size_t second_len = strlen(second);

  // Allocate the new buffer, size it to hold all we want to put in there +1.
  size_t result_len =  strlen(file_name) + first_len - first_replace_len + second_len - second_replace_len;
  result = NEW_C_HEAP_ARRAY(char, result_len + 1, mtLogging);

  // Assemble the strings
  size_t file_name_pos = 0;
  size_t i = 0;
  while (i < result_len) {
    if (file_name_pos == first_pos) {
      // We are in the range of the first placeholder
      strcpy(result + i, first);
      // Bump output buffer position with length of replacing string
      i += first_len;
      // Bump source buffer position to skip placeholder
      file_name_pos += first_replace_len;
    } else if (file_name_pos == second_pos) {
      // We are in the range of the second placeholder
      strcpy(result + i, second);
      i += second_len;
      file_name_pos += second_replace_len;
    } else {
      // Else, copy char by char of the original file
      result[i] = file_name[file_name_pos++];
      i++;
    }
  }
  // Add terminating char
  result[result_len] = '\0';
  return result;
}