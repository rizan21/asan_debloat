/* TEMPLATE GENERATED TESTCASE FILE
Filename: CWE78_OS_Command_Injection__wchar_t_console_w32_execvp_52b.c
Label Definition File: CWE78_OS_Command_Injection.strings.label.xml
Template File: sources-sink-52b.tmpl.c
*/
/*
 * @description
 * CWE: 78 OS Command Injection
 * BadSource: console Read input from the console
 * GoodSource: Fixed string
 * Sink: w32_execvp
 *    BadSink : execute command with wexecvp
 * Flow Variant: 52 Data flow: data passed as an argument from one function to another to another in three different source files
 *
 * */

#include "std_testcase.h"

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
#define EXECVP _wexecvp

/* all the sinks are the same, we just want to know where the hit originated if a tool flags one */

#ifndef OMITBAD

/* bad function declaration */
void CWE78_OS_Command_Injection__wchar_t_console_w32_execvp_52c_badSink(wchar_t * data);

void CWE78_OS_Command_Injection__wchar_t_console_w32_execvp_52b_badSink(wchar_t * data)
{
    CWE78_OS_Command_Injection__wchar_t_console_w32_execvp_52c_badSink(data);
}

#endif /* OMITBAD */

#ifndef OMITGOOD

/* good function declaration */
void CWE78_OS_Command_Injection__wchar_t_console_w32_execvp_52c_goodG2BSink(wchar_t * data);

/* goodG2B uses the GoodSource with the BadSink */
void CWE78_OS_Command_Injection__wchar_t_console_w32_execvp_52b_goodG2BSink(wchar_t * data)
{
    CWE78_OS_Command_Injection__wchar_t_console_w32_execvp_52c_goodG2BSink(data);
}

#endif /* OMITGOOD */
