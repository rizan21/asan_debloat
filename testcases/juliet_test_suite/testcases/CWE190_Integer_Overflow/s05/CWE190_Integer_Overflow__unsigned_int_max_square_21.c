/* TEMPLATE GENERATED TESTCASE FILE
Filename: CWE190_Integer_Overflow__unsigned_int_max_square_21.c
Label Definition File: CWE190_Integer_Overflow.label.xml
Template File: sources-sinks-21.tmpl.c
*/
/*
 * @description
 * CWE: 190 Integer Overflow
 * BadSource: max Set data to the max value for unsigned int
 * GoodSource: Set data to a small, non-zero number (two)
 * Sinks: square
 *    GoodSink: Ensure there will not be an overflow before squaring data
 *    BadSink : Square data, which can lead to overflow
 * Flow Variant: 21 Control flow: Flow controlled by value of a static global variable. All functions contained in one file.
 *
 * */

#include "std_testcase.h"

#include <math.h>

#ifndef OMITBAD

/* The static variable below is used to drive control flow in the sink function */
static int badStatic = 0;

static void badSink(unsigned int data)
{
    if(badStatic)
    {
        {
            /* POTENTIAL FLAW: if (data*data) > UINT_MAX, this will overflow */
            unsigned int result = data * data;
            printUnsignedLine(result);
        }
    }
}

void CWE190_Integer_Overflow__unsigned_int_max_square_21_bad()
{
    unsigned int data;
    data = 0;
    /* POTENTIAL FLAW: Use the maximum size of the data type */
    data = UINT_MAX;
    badStatic = 1; /* true */
    badSink(data);
}

#endif /* OMITBAD */

#ifndef OMITGOOD

/* The static variables below are used to drive control flow in the sink functions. */
static int goodB2G1Static = 0;
static int goodB2G2Static = 0;
static int goodG2BStatic = 0;

/* goodB2G1() - use badsource and goodsink by setting the static variable to false instead of true */
static void goodB2G1Sink(unsigned int data)
{
    if(goodB2G1Static)
    {
        /* INCIDENTAL: CWE 561 Dead Code, the code below will never run */
        printLine("Benign, fixed string");
    }
    else
    {
        /* FIX: Add a check to prevent an overflow from occurring */
        if (abs((long)data) <= (long)sqrt((double)UINT_MAX))
        {
            unsigned int result = data * data;
            printUnsignedLine(result);
        }
        else
        {
            printLine("data value is too large to perform arithmetic safely.");
        }
    }
}

static void goodB2G1()
{
    unsigned int data;
    data = 0;
    /* POTENTIAL FLAW: Use the maximum size of the data type */
    data = UINT_MAX;
    goodB2G1Static = 0; /* false */
    goodB2G1Sink(data);
}

/* goodB2G2() - use badsource and goodsink by reversing the blocks in the if in the sink function */
static void goodB2G2Sink(unsigned int data)
{
    if(goodB2G2Static)
    {
        /* FIX: Add a check to prevent an overflow from occurring */
        if (abs((long)data) <= (long)sqrt((double)UINT_MAX))
        {
            unsigned int result = data * data;
            printUnsignedLine(result);
        }
        else
        {
            printLine("data value is too large to perform arithmetic safely.");
        }
    }
}

static void goodB2G2()
{
    unsigned int data;
    data = 0;
    /* POTENTIAL FLAW: Use the maximum size of the data type */
    data = UINT_MAX;
    goodB2G2Static = 1; /* true */
    goodB2G2Sink(data);
}

/* goodG2B() - use goodsource and badsink */
static void goodG2BSink(unsigned int data)
{
    if(goodG2BStatic)
    {
        {
            /* POTENTIAL FLAW: if (data*data) > UINT_MAX, this will overflow */
            unsigned int result = data * data;
            printUnsignedLine(result);
        }
    }
}

static void goodG2B()
{
    unsigned int data;
    data = 0;
    /* FIX: Use a small, non-zero value that will not cause an overflow in the sinks */
    data = 2;
    goodG2BStatic = 1; /* true */
    goodG2BSink(data);
}

void CWE190_Integer_Overflow__unsigned_int_max_square_21_good()
{
    goodB2G1();
    goodB2G2();
    goodG2B();
}

#endif /* OMITGOOD */

/* Below is the main(). It is only used when building this testcase on
   its own for testing or for building a binary to use in testing binary
   analysis tools. It is not used when compiling all the testcases as one
   application, which is how source code analysis tools are tested. */

#ifdef INCLUDEMAIN

int main(int argc, char * argv[])
{
    /* seed randomness */
    srand( (unsigned)time(NULL) );
#ifndef OMITGOOD
    printLine("Calling good()...");
    CWE190_Integer_Overflow__unsigned_int_max_square_21_good();
    printLine("Finished good()");
#endif /* OMITGOOD */
#ifndef OMITBAD
    printLine("Calling bad()...");
    CWE190_Integer_Overflow__unsigned_int_max_square_21_bad();
    printLine("Finished bad()");
#endif /* OMITBAD */
    return 0;
}

#endif
