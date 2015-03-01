#ifndef XPOSED_LOGCAT_H_
#define XPOSED_LOGCAT_H_

#define XPOSEDLOG     XPOSED_DIR "log/error.log"
#define XPOSEDLOG_OLD XPOSEDLOG ".old"
#define XPOSEDLOG_MAX_SIZE 5*1024*1024

namespace xposed {
namespace logcat {

    void start();

}  // namespace logcat
}  // namespace xposed

#endif /* XPOSED_LOGCAT_H_ */
