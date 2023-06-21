/*
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

/*
 * @test
 * @bug 8293336
 * @summary Test for archiving resolved invokedynamic call sites
 * @requires vm.cds.write.archived.java.heap
 * @modules java.base/sun.invoke.util java.logging
 * @library /test/jdk/lib/testlibrary /test/lib
 *          /test/hotspot/jtreg/runtime/cds/appcds
 *          /test/hotspot/jtreg/runtime/cds/appcds/test-classes
 * @build PrelinkedStringConcat
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar app.jar ConcatA ConcatA$DummyClass ConcatB
 * @run driver PrelinkedStringConcat
 */


// NOTE: to run a subset of the tests, use something like
//
// jtreg .... -vmoptions:-DPrelinkedStringConcat.test.only='(1)|(2)' \
//            -vmoptions:-DPrelinkedStringConcat.test.skip='2' PrelinkedStringConcat.java
//
// A regexp can be specified in these two properties. Note that the specified regexp must be a full match.
// E.g., -DPrelinkedStringConcat.test.only='1.*' matches "1" and "12", but does NOT match "21".
// (Search for ")$" in the code below)
//
// Also, some tests may be forced to be skipped. To run them, edit the variable forceSkip below.

import java.io.FileWriter;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import jdk.test.lib.cds.CDSOptions;
import jdk.test.lib.cds.CDSTestUtils;
import jdk.test.lib.helpers.ClassFileInstaller;
import jdk.test.lib.process.OutputAnalyzer;

public class PrelinkedStringConcat {
    static final String appJar = ClassFileInstaller.getJarPath("app.jar");
    static final String mainClass_ConcatA = ConcatA.class.getName();
    static final String mainClass_ConcatB = ConcatB.class.getName();
    static final String mainClass_javac = "com.sun.tools.javac.Main";
    static Pattern testOnlyPattern = null; // matches testNumber
    static Pattern testSkipPattern = null; // matches testNumber
    static OutputAnalyzer output = null;

    // Force some tests to be disabled during development.
    static String forceSkip = ".*javac.*"; // matches testNote -- the javac test isn't working yet ...
    static Pattern forceSkipPattern = null;
    static int testNumber = 0;

    public static void main(String[] args) throws Exception {
        setup();

        // ------------------------------------------------------------
        test("\"LIT\" + (String)b", mainClass_ConcatA, "a");
        checkExec("LIT222");

        // ------------------------------------------------------------
        test("(String)a + (String)b", mainClass_ConcatA, "b");
        checkExec("aaa222");

        // ------------------------------------------------------------
        test("(String)a + (int)b", mainClass_ConcatA, "c");
        checkExec("aaa333");

        // ------------------------------------------------------------
        test("See if -XX:ArchiveInvokeDynamicFilter=ConcatB works", mainClass_ConcatB, "B1");
        checkExec("ConcatBLIT333");

        // ------------------------------------------------------------
        test("Run ConcatB.foo() without dump-time resolution of its invokedynamic callsite", mainClass_ConcatB, "", "B1");
        checkExec("ConcatBLIT333", /* lambdaFormsMustBeArchived*/ false);

        // ------------------------------------------------------------
        test("WithAOT (no loop) for \"LIT\" + (String)b", mainClass_ConcatA, "a", "a",  /*testAOT = */true);
        checkExec("LIT222");
        shouldUseDynamicArchive();

        // ------------------------------------------------------------
        test("NoAOT (with loop) \"LIT\" + (String)b", mainClass_ConcatA, "loopa");
        checkExec("LITL");

        // ------------------------------------------------------------
        test("WithAOT (with loop) for \"LIT\" + (String)b", mainClass_ConcatA, "loopa", "loopa",  /*testAOT = */true);
        checkExec("LITL");
        shouldUseDynamicArchive();

        // ------------------------------------------------------------
        testJavaC();
    }

    // FIXME -- this case is not working yet. It throws exception while initializing the core libs.
    // Change forceSkip = null to run this case.
    static void testJavaC() throws Exception {
        String sourceFile = "Test.java";
        try (FileWriter fw = new FileWriter(sourceFile)) {
            fw.write("public class Test {\n");
            for (int i = 0; i < 3000; i++) {
                fw.write("    private static final int arr_" + i + "[] = {1, 2, 3};\n");
                fw.write("    private static int method_" + i + "() { return arr_" + i + "[1];}\n");
            }
            fw.write("}\n");
        }
        test("NoAOT - use javac with archived MethodTypes and LambdaForms", mainClass_javac, sourceFile);
        checkExec(null, /*lambdaFormsMustBeArchived*/true);
    }

    static void checkExec(String expectedOutput) throws Exception {
        checkExec(expectedOutput, /* lambdaFormsMustBeArchived*/ true);
    }

    static void checkExec(String expectedOutput, boolean lambdaFormsMustBeArchived)  throws Exception {
        if (output == null) { // test may be skipped
            return;
        }
        if (expectedOutput != null) {
            TestCommon.checkExecReturn(output, 0, true, "OUTPUT = " + expectedOutput);
        }
        if (lambdaFormsMustBeArchived) {
            output.shouldMatch("LambdaForm[$]((MH)|(DMH))/0x[0-9]+ source: shared objects file");
            output.shouldNotMatch("LambdaForm[$]MH/0x[0-9]+ source: __JVM_LookupDefineClass__");
            output.shouldNotMatch("LambdaForm[$]DMH/0x[0-9]+ source: __JVM_LookupDefineClass__");
        }
    }

    static void shouldMatch(String pattern) throws Exception {
        if (output != null) { // test may be skipped
            output.shouldMatch(pattern);
        }
    }

    static void shouldUseDynamicArchive() throws Exception {
        shouldMatch("Opened archive PrelinkedStringConcat-[0-9]+-dyn.jsa");
    }

    static void setup() throws Exception {
        String testOnly = System.getProperty("PrelinkedStringConcat.test.only");
        if (testOnly != null) {
            testOnlyPattern = Pattern.compile("^(" + testOnly + ")$");
        }
        String testSkip = System.getProperty("PrelinkedStringConcat.test.skip");
        if (testSkip != null) {
            testSkipPattern = Pattern.compile("^(" + testSkip + ")$");
        }

        if (forceSkip != null) {
            forceSkipPattern = Pattern.compile("^(" + forceSkip + ")$");
        }
    }

    static boolean shouldTest(String s) {
        if (testOnlyPattern != null) {
            Matcher matcher = testOnlyPattern.matcher(s);
            if (!matcher.find()) {
                return false;
            }
        }
        if (testSkipPattern != null) {
            Matcher matcher = testSkipPattern.matcher(s);
            if (matcher.find()) {
                return false;
            }
        }

        return true;
    }

    // Run the test program with the same arg for both training run and production run
    static void test(String testNote, String mainClass, String arg) throws Exception {
        test(testNote, mainClass, arg, arg, /*testAOT = */ false);
    }

    static void test(String testNote, String mainClass, String trainingArg, String productionArg) throws Exception {
        test(testNote, mainClass, trainingArg, productionArg, /*testAOT = */ false);
    }

    static void test(String testNote, String mainClass, String trainingArg, String productionArg, boolean testAOT) throws Exception {
        output = null;
        testNumber ++;
        String skipBy = null;

        if (forceSkipPattern != null) {
            Matcher matcher = forceSkipPattern.matcher(testNote);
            if (matcher.find()) {
                skipBy = " ***** (hard coded) Skipped by test note";
            }
        }
        if (skipBy == null && !shouldTest("" + testNumber)) {
            skipBy = " ***** Skipped by test number";
        }

        if (skipBy != null) {
            System.out.println("         Test : #" + testNumber + ", " + testNote + skipBy);
            return;
        }

        System.out.println("==================================================================");
        System.out.println("         Test : #" + testNumber + ", " + testNote);
        System.out.println("  trainingArg : " + trainingArg);
        System.out.println("productionArg : " + productionArg);
        System.out.println("      testAOT : " + testAOT);
        System.out.println("vvvv==========================================================vvvv");
        String s = "PrelinkedStringConcat-" + testNumber;
        String classList = s + ".classlist";
        String archiveName = s + ".jsa";

        // Create classlist
        CDSTestUtils.dumpClassList(classList, "-cp", appJar, mainClass, trainingArg);

        // Dump archive
        CDSOptions opts = (new CDSOptions())
            .addPrefix("-XX:SharedClassListFile=" + classList,
                       "-XX:+ArchiveInvokeDynamic",
                       "-XX:ArchiveInvokeDynamicFilter=ConcatA",
                       "-XX:ArchiveInvokeDynamicFilter=ConcatB",
                       "-cp", appJar,
                       "-Xlog:cds,cds+class=debug")
            .setArchiveName(archiveName);
        output = CDSTestUtils.createArchiveAndCheck(opts);
        TestCommon.checkExecReturn(output, 0, true);

        if (!testAOT) {
            // Run with archive
            CDSOptions runOpts = (new CDSOptions())
                .addPrefix("-cp", appJar, "-Xlog:class+load", "-Xlog:cds=debug",
                           "-Xlog:methodhandles")
                .setArchiveName(archiveName)
                .setUseVersion(false)
                .addSuffix(mainClass)
                .addSuffix(productionArg);
            output = CDSTestUtils.runWithArchive(runOpts);
            TestCommon.checkExecReturn(output, 0, true);
        } else {
            // TODO -- for the time being, AOT is only available with dynamic archive.
            String dynamicArchiveName = s + "-dyn.jsa";
            String sharedCodeArchive = s + ".sca";
            CDSOptions dynDumpOpts = (new CDSOptions())
                .addPrefix("-cp", appJar, "-Xlog:class+load", "-Xlog:cds=debug",
                           "-Xlog:methodhandles")
                .setArchiveName(archiveName)
                .setUseVersion(false)
                .addSuffix("-XX:ArchiveClassesAtExit=" + dynamicArchiveName)
                .addSuffix("-Xlog:cds+class=debug")
                .addSuffix("-XX:+StoreSharedCode")
                .addSuffix("-XX:SharedCodeArchive=" + sharedCodeArchive)
                .addSuffix("-XX:ReservedSharedCodeSize=100M")
                .addSuffix("-Xlog:sca*=trace");
            if (mainClass.equals(mainClass_ConcatA)) {
                // Hard-code the printing of loopA for now
                dynDumpOpts
                    .addSuffix("-XX:CompileCommand=print,*::loopA")
                    .addSuffix("-XX:+PrintAssembly");
                if (false) {
                    dynDumpOpts.addSuffix("-XX:-TieredCompilation");
                }
            }
            // The main class name and arguments
            dynDumpOpts
                .addSuffix(mainClass)
                .addSuffix(productionArg)
                .addSuffix("load-extra-class");
            output = CDSTestUtils.runWithArchive(dynDumpOpts);
            TestCommon.checkExecReturn(output, 0, true);

            // Run with dynamic archive and AOT code cache
            CDSOptions runOpts = (new CDSOptions())
                .addPrefix("-cp", appJar, "-Xlog:class+load", "-Xlog:cds=debug",
                           "-Xlog:methodhandles")
                .setArchiveName(dynamicArchiveName)
                .addSuffix("-XX:+LoadSharedCode")
                .addSuffix("-XX:SharedCodeArchive=" + sharedCodeArchive)
                .addSuffix("-Xlog:sca*=trace")
                .setUseVersion(false)
                .addSuffix(mainClass)
                .addSuffix(productionArg);
            output = CDSTestUtils.runWithArchive(runOpts);
            TestCommon.checkExecReturn(output, 0, true);
        }
    }
}

class ConcatA {
    public static void main(String args[]) throws Exception {
        if (args[0].equals("a")) {
            foo("222");
            System.out.print("OUTPUT = ");
            System.out.println(x); // Avoid using + in diagnostic output.
        } else if (args[0].equals("b")) {
            bar("aaa", "222");
            System.out.print("OUTPUT = ");
            System.out.println(x); // Avoid using + in diagnostic output.
        } else if (args[0].equals("c")) {
            baz("aaa", 333);
            System.out.print("OUTPUT = ");
            System.out.println(x); // Avoid using + in diagnostic output.
        } else if (args[0].equals("loopa")) {
            loopa();
            loopa();
            System.out.print("OUTPUT = ");
            System.out.println(x); // Avoid using + in diagnostic output.
        }

        if (args.length > 1 && args[1].equals("load-extra-class")) {
            // Work around "There is no class to be included in the dynamic archive." problem, where the
            // dynamic archive is not generated.
            DummyClass.doit();
        }
    }

    static void loopa() {
        for (int i = 0; i < 100000; i++) {
            foo("L");
        }
    }

    static String x;
    static void foo(String b) {
        x = "LIT" + b;
    }
    static void bar(String a, String b) {
        x = a + b;
    }
    static void baz(String a, int b) {
        x = a + b;
    }

    static class DummyClass {
        static void doit() {}
    }
}


class ConcatB {
    public static void main(String args[]) throws Exception {
        if (args[0].equals("B1")) {
            foo("333");
            System.out.print("OUTPUT = ");
            System.out.println(x); // Avoid using + in diagnostic output.
        }
    }

    static String x;
    static void foo(String b) {
        x = "ConcatBLIT" + b;
    }
}
