/* TEMPLATE GENERATED TESTCASE FILE
Filename: CWE78_OS_Command_Injection__wchar_t_environment_w32_spawnv_81_goodG2B.cpp
Label Definition File: CWE78_OS_Command_Injection.strings.label.xml
Template File: sources-sink-81_goodG2B.tmpl.cpp
*/
/*
 * @description
 * CWE: 78 OS Command Injection
 * BadSource: environment Read input from an environment variable
 * GoodSource: Fixed string
 * Sinks: w32_spawnv
 *    BadSink : execute command with wspawnv
 * Flow Variant: 81 Data flow: data passed in a parameter to an virtual method called via a reference
 *
 * */
#ifndef OMITGOOD

#include "std_testcase.h"
#include "CWE78_OS_Command_Injection__wchar_t_environment_w32_spawnv_81.h"

#include <process.h>

namespace CWE78_OS_Command_Injection__wchar_t_environment_w32_spawnv_81
{

void CWE78_OS_Command_Injection__wchar_t_environment_w32_spawnv_81_goodG2B::action(wchar_t * data) const
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
