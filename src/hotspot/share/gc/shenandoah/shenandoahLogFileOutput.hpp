//
// Created by Young, Christian on 6/28/21.
//

#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHLOGFILEOUTPUT_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHLOGFILEOUTPUT_HPP

#include "logging/logFileStreamOutput.hpp"
#include "logging/logFileOutput.hpp"
#include "utilities/globalDefinitions.hpp"
#include "runtime/perfData.inline.hpp"


// Log file output to capture Shenandoah GC data.
//class ShenandoahLogFileOutput : public LogFileStreamOutput {
//  private:
//    static const char* const FileOpenMode;
//    static const char* const PidFilenamePlaceholder;
//    static const char* const TimestampFilenamePlaceholder;
//    static const char* const TimestampFormat;
//    static const size_t StartTimeBufferSize = 20;
//    static const size_t PidBufferSize = 21;
//    static char         _pid_str[PidBufferSize];
//    static char         _vm_start_time_str[StartTimeBufferSize];
//
//    bool _write_error_is_shown;
//
//    const char* _name;
//    char* _file_name;
//
//    size_t _snapshot_count;
//    size_t  _current_size;
//
//    bool parse_options(const char* options, outputStream* errstream);
//    char *make_file_name(const char* file_name, const char* pid_string, const char* timestamp_string);
//    bool flush();
//
//  public:
//    ShenandoahLogFileOutput(const char *name);
//    virtual ~ShenandoahLogFileOutput();
//    virtual bool initialize(const char* options, outputStream* errstream);
//    virtual int write(const LogDecorations& decorations, const char* msg);
//    virtual int write(LogMessageBuffer::Iterator msg_iterator);
//    int write_snapshot(PerfLongVariable** regions,
//                       PerfLongVariable* ts,
//                       PerfLongVariable* status,
//                       size_t num_regions,
//                       size_t rs);
//
//      virtual const char* name() const {
//        return _name;
//      }
//};

class ShenandoahLogFileOutput : public LogFileOutput {
private:
    static const char* const FileOpenMode;
    static const char* const PidFilenamePlaceholder;
    static const char* const TimestampFilenamePlaceholder;
    static const char* const TimestampFormat;
    static const size_t StartTimeBufferSize = 20;
    static const size_t PidBufferSize = 21;
    static char         _pid_str[PidBufferSize];
    static char         _vm_start_time_str[StartTimeBufferSize];

    const char* _name;
    char* _file_name;

    size_t _snapshot_count;
    size_t  _current_size;

    bool _write_error_is_shown;

    bool parse_options(const char* options, outputStream* errstream);
    char *make_file_name(const char* file_name, const char* pid_string, const char* timestamp_string);

    bool flush();

public:
    ShenandoahLogFileOutput(const char *name);
    virtual ~ShenandoahLogFileOutput();

    // Implemented pure virtual methods
    virtual bool initialize(const char* options, outputStream* errstream);
    virtual int write(const LogDecorations& decorations, const char* msg);
    virtual int write(LogMessageBuffer::Iterator msg_iterator);
    //

    bool initialize();

    int write_snapshot(PerfLongVariable** regions,
                       PerfLongVariable* ts,
                       PerfLongVariable* status,
                       size_t num_regions,
                       size_t rs);

    virtual const char* name() const {
      return _name;
    }

    static const char* const Prefix;
    static void set_file_name_parameters(jlong start_time);
};
#endif //SHARE_GC_SHENANDOAH_SHENANDOAHLOGFILEOUTPUT_HPP
