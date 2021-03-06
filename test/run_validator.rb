#!/usr/bin/env ruby

binaryDir = ENV["BINARY_DIR"]

# arguments are a string that must match the test name
substrpat = ARGV.length ? ARGV[0] : ""

thisDir = File.dirname(__FILE__)
casesDir = File.join(thisDir, "validator")
verifyBin = File.join(binaryDir,  "validator",  "orderly_verify")
if !File.executable? verifyBin
  throw "Can't find validator test binary: #{verifyBin}"
end
passed = 0
total = 0
files = Dir.glob(File.join(casesDir,"**.{pass,fail}","*.test"))
puts "1..#{files.length}"
puts "#Running validator tests: "
puts "#(containing '#{substrpat}' in name)" if substrpat && substrpat.length > 0
Dir.glob(File.join(casesDir, "*.orderly")).each { |f| 
  next if substrpat && substrpat.length > 0 && !f.include?(substrpat)
  [
    [ "Invalid case", f.sub(/orderly$/, "fail"), verifyBin, 1 ],
    [ "Valid case", f.sub(/orderly$/, "pass"),   verifyBin, 0 ]
  ].each { |testType|
    what, pfDir, program, exitCode = *testType
    Dir.glob(File.join(pfDir, "*.test")).each { |textfile| 
      total += 1
      wantFile = textfile + ".want"
      gotFile = textfile + ".got"
      got = ""
      explanation = "#{what} for #{textfile}:\t" ;
      ENV['ORDERLY_SCHEMA'] = IO.readlines(f,'').to_s
      IO.popen(program, "w+") { |lb|
        File.open(textfile, "r").each {|l| lb.write(l)}
        lb.close_write
        got = lb.read
      }
      if ($?.exitstatus != exitCode) 
        puts "not ok #{total} - #{explanation}";
        puts "# got bad exit code '#{$?.exitstatus}', expected '#{exitCode}'"
      else
        if File.exist? wantFile
          want = IO.read(wantFile)
          if (got == want)
            puts "ok #{total} - #{explanation}"
            passed += 1
          else
            puts "not ok #{total}"
            puts "#<<<want<<<"
            puts want.gsub(/^/,"#")
            puts "#========"
            puts got.gsub(/^/,"#")
            puts "#>>got>>"
          end
        else
          puts "ok #{total} - #{explanation}"
          passed += 1
        end
      end
    }
  }
}
puts "# #{passed}/#{total} tests successful"
exit passed == total

