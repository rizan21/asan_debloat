/* TEMPLATE GENERATED TESTCASE FILE
Filename: CWE78_OS_Command_Injection__wchar_t_listen_socket_w32_spawnv_82_goodG2B.cpp
Label Definition File: CWE78_OS_Command_Injection.strings.label.xml
Template File: sources-sink-82_goodG2B.tmpl.cpp
*/
/*
 * @description
 * CWE: 78 OS Command Injection
 * BadSource: listen_socket Read data using a listen socket (server side)
 * GoodSource: Fixed string
 * Sinks: w32_spawnv
 *    BadSink : execute command with wspawnv
 * Flow Variant: 82 Data flow: data passed in a parameter to an virtual method called via a pointer
 *
 * */
#ifndef OMITGOOD

#include "std_testcase.h"
#include "CWE78_OS_Command_Injection__wchar_t_listen_socket_w32_spawnv_82.h"

#include <process.h>

namespace CWE78_OS_Command_Injection__wchar_t_listen_socket_w32_spawnv_82
{

void CWE78_OS_Command_Injection__wchar_t_listen_socket_w32_spawnv_82_goodG2B::action(wchar_t * data)
{
    {
        wchar_t *args[] = {COMMAND_INT_PATH, COMMAND_ARG1, COMMAND_ARG2, COMMAND_ARG3, NULL};
        /* wspawnv - specify the path where the command is located */
        /* POTENTIAL FLAW: Execute command without validating input possibly leading to command injection */
        _wspawnv(_P_WAIT, COMMAND_INT_PATH, args);
    }
}

}
#endif /* OMITGOOD */
