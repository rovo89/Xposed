#ifndef XPOSED_LOGCAT_H_
#define XPOSED_LOGCAT_H_

#define XPOSEDLOG            XPOSED_DIR "log/error.log"
#define XPOSEDLOG_OLD        XPOSEDLOG ".old"
#define XPOSEDLOG_CONF_ALL   XPOSED_DIR "conf/log_all"
#define XPOSEDLOG_MAX_SIZE   5*1024*1024

namespace xposed {
namespace logcat {

    void printStartupMarker();
    void start();

}  // namespace logcat
}  // namespace xposed

#endif /* XPOSED_LOGCAT_H_ */
