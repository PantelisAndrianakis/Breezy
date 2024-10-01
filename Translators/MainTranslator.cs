// Author: Pantelis Andrianakis
// Creation Date: October 1st 2024

using System.Text.RegularExpressions;

namespace Breezy.Translators
{
	public class MainTranslator
	{
		public static string Translate(string sourceCode)
		{
			// Use regex to find the full void main() method and replace it with int main().
			string cppCode = Regex.Replace(sourceCode, @"void\s+main\s*\(\)", "int main()");

			// Modify the regex pattern to target only the main method and its closing brace.
			cppCode = Regex.Replace(cppCode, @"(int\s+main\s*\(\)\s*\{[\s\S]*?)\}\s*$", "$1\treturn 0;\n}");

			// Use regex to handle void main(int argc, string* argv[]) and replace it with int main(int argc, char* argv[]).
			cppCode = Regex.Replace(cppCode, @"void\s+main\s*\(int\s+argc\s*,\s*string\*\s+argv\[\]\)", "int main(int argc, char* argv[])");

			// Add a conversion from argv[] (char*) to args[] (std::string) inside main function.
			cppCode = Regex.Replace(cppCode, @"int\s+main\s*\(int\s+argc,\s*char\*\s*argv\[\]\s*\)\s*\{", match =>
			{
				return match.Value + "\n\tstd::string args[argc];\n\tfor(int i = 0; i < argc; ++i) args[i] = std::string(argv[i]);";
			});

			// Modify the regex pattern to target the main method with arguments and ensure return 0; is added before the closing brace.
			cppCode = Regex.Replace(cppCode, @"(int\s+main\s*\(int\s+argc,\s*char\*\s*argv\[\]\s*\{[\s\S]*?)\}\s*$", "$1\treturn 0;\n}");

			return cppCode;
		}
	}
}
