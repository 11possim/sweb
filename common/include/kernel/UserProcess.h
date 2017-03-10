#pragma once

#include "Thread.h"

/**
 * @class UserProcess
 * Thread used to execute a file in minixfs.
 * NOTE: this process is only ONE thread; if you want to implement mulithreading,
 * each thread has to be inherited from @ref Thread
 */
class UserProcess : public Thread
{
  public:
    /**
     * Constructor
     * @param minixfs_filename filename of the file in minixfs to execute
     * @param fs_info filesysteminfo-object to be used
     * @param terminal_number the terminal to run in (default 0)
     *
     */
    UserProcess(ustl::string minixfs_filename, FileSystemInfo *fs_info, uint32 terminal_number = 0);

    virtual ~UserProcess();

    virtual void Run(); // not used

  private:
    int32 fd_;
};

