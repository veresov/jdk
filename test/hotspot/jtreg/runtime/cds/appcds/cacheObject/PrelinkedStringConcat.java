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
 * @run driver jdk.test.lib.helpers.ClassFileInstaller -jar app.jar ConcatA
 * @run driver PrelinkedStringConcat
 */

import jdk.test.lib.cds.CDSOptions;
import jdk.test.lib.cds.CDSTestUtils;
import jdk.test.lib.helpers.ClassFileInstaller;
import jdk.test.lib.process.OutputAnalyzer;

public class PrelinkedStringConcat {
    static final String appJar = ClassFileInstaller.getJarPath("app.jar");
    static final String mainClass = ConcatA.class.getName();
    static final String classList = mainClass + ".classlist";
    static final String archiveName = mainClass + ".jsa";

    public static void main(String[] args) throws Exception {
        // Create classlist
        CDSTestUtils.dumpClassList(classList, "-cp", appJar, mainClass);

        // Dump archive
        CDSOptions opts = (new CDSOptions())
            .addPrefix("-XX:SharedClassListFile=" + classList,
                       "-XX:+ArchiveInvokeDynamic",
                       "-cp", appJar,
                       "-Xlog:cds,cds+class=debug")
            .setArchiveName(archiveName);
        OutputAnalyzer output = CDSTestUtils.createArchiveAndCheck(opts);
        TestCommon.checkExecReturn(output, 0, true);

        // Run with archive
        CDSOptions runOpts = (new CDSOptions())
            .addPrefix("-cp", appJar, "-Xlog:cds=debug",
                       "-Xlog:methodhandles")
            .setArchiveName(archiveName)
            .setUseVersion(false)
            .addSuffix(mainClass);
        output = CDSTestUtils.runWithArchive(runOpts);
        TestCommon.checkExecReturn(output, 0, true,
                                   "OUTPUT = a222");
    }
}

class ConcatA {
    public static void main(String args[]) throws Exception {
        foo("222");
        System.out.print("OUTPUT = ");
        System.out.println(x);
    }

    static String x;
    static void foo(String b) {
        x = "a" + b;
    }
}
