/* TEMPLATE GENERATED TESTCASE FILE
Filename: CWE78_OS_Command_Injection__wchar_t_console_w32_execv_73b.cpp
Label Definition File: CWE78_OS_Command_Injection.strings.label.xml
Template File: sources-sink-73b.tmpl.cpp
*/
/*
 * @description
 * CWE: 78 OS Command Injection
 * BadSource: console Read input from the console
 * GoodSource: Fixed string
 * Sinks: w32_execv
 *    BadSink : execute command with wexecv
 * Flow Variant: 73 Data flow: data passed in a list from one function to another in different source files
 *
 * */

#include "std_testcase.h"
#include <list>

#include <wchar.h>

#ifdef _WIN32
#define COMMAND_INT_PATH L"%WINDIR%\\system32\\cmd.exe"
#define COMMAND_INT L"cmd.exe"
#define COMMAND_ARG1 L"/c"
#define COMMAND_ARG2 L"dir"
#define COMMAND_ARG3 data
#else /* NOT _WIN32 */
#include <unistd.h>
#define COMMAND_INT_PATH L"/bin/sh"
#define COMMAND_INT L"sh"
#define COMMAND_ARG1 L"ls"
#define COMMAND_ARG2 L"-la"
#define COMMAND_ARG3 data
#endif

#include <process.h>
#define EXECV _wexecv

using namespace std;

namespace CWE78_OS_Command_Injection__wchar_t_console_w32_execv_73
{

#ifndef OMITBAD

void badSink(list<wchar_t *> dataList)
{
    /* copy data out of dataList */
    wchar_t * data = dataList.back();
    {
        wchar_t *args[] = {COMMAND_INT_PATH, COMMAND_ARG1, COMMAND_ARG2, COMMAND_ARG3, NULL};
        /* wexecv - specify the path where the command is located */
        /* POTENTIAL FLAW: Execute command without validating input possibly leading to command injection */
        EXECV(COMMAND_INT_PATH, args);
    }
}

#endif /* OMITBAD */

#ifndef OMITGOOD

/* goodG2B uses the GoodSource with the BadSink */
void goodG2BSink(list<wchar_t *> dataList)
{
    wchar_t * data = dataList.back();
    {
        wchar_t *args[] = {COMMAND_INT_PATH, COMMAND_ARG1, COMMAND_ARG2, COMMAND_ARG3, NULL};
        /* wexecv - specify the path where the command is located */
        /* POTENTIAL FLAW: Execute command without validating input possibly leading to command injection */
        EXECV(COMMAND_INT_PATH, args);
    }
}

#endif /* OMITGOOD */

} /* close namespace */
