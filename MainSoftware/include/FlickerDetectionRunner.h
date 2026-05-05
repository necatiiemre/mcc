#ifndef FLICKER_DETECTION_RUNNER_H
#define FLICKER_DETECTION_RUNNER_H

#include <string>
#include <sys/types.h>

class FlickerDetectionRunner
{
public:
    FlickerDetectionRunner();
    ~FlickerDetectionRunner();

    /**
     * @brief Spawn the local FlickerDetection binary with CMC defaults.
     *        Stdout/stderr are redirected to log_path.
     * @return true if the child process was started.
     */
    bool startForCmc(const std::string& log_path);

    /**
     * @brief Send SIGTERM to the running FlickerDetection process and wait
     *        until it exits (with a bounded grace period before SIGKILL).
     */
    void stop();

    bool isRunning() const;

private:
    pid_t m_pid;
};

#endif // FLICKER_DETECTION_RUNNER_H
