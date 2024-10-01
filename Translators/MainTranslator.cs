// Author: Pantelis Andrianakis
// Creation Date: October 1st 2024

using System.Text.RegularExpressions;

namespace Breezy.Translators
{
	public class MainTranslator
	{
		public static string Translate(string sourceCode)
		{
			string cppCode = sourceCode;

			if (cppCode.Contains("void main()"))
			{
				// Handle void main() and replace it with int main().
				cppCode = Regex.Replace(cppCode, @"void\s+main\s*\(\)", "int main()");

				// Insert the std::string args logic inside main() with arguments, if applicable.
				cppCode = Regex.Replace(cppCode, @"int\s+main\s*\(int\s+argc,\s*char\*\s*argv\[\]\s*\)\s*\{", match =>
				{
					return match.Value + "\n\tstd::string args[argc];\n\tfor(int i = 0; i < argc; ++i) args[i] = std::string(argv[i]);";
				});

				// Final regex to ensure return 0; is added just before the final closing brace of main(), even with nested braces.
				cppCode = Regex.Replace(cppCode, @"(int\s+main\s*\([\s\S]*?\{[\s\S]*?)(\n\})", "$1\treturn 0;\n}");
			}
			else if (cppCode.Contains("void main(int argc, string* argv[])"))
			{
				// Handle void main(int argc, string* argv[]) and replace it with int main(int argc, char* argv[]).
				cppCode = Regex.Replace(cppCode, @"void\s+main\s*\(int\s+argc\s*,\s*string\*\s+argv\[\]\)", "int main(int argc, char* argv[])");

				// Add a conversion from argv[] (char*) to args[] (std::string) inside main function with arguments.
				cppCode = Regex.Replace(cppCode, @"int\s+main\s*\(int\s+argc,\s*char\*\s*argv\[\]\s*\)\s*\{", match =>
				{
					return match.Value + "\n\tstd::string args[argc];\n\tfor(int i = 0; i < argc; ++i) args[i] = std::string(argv[i]);";
				});

				// Final regex to ensure return 0; is added at the end of int main(int argc, char* argv[]), and handle braces correctly.
				cppCode = Regex.Replace(cppCode, @"(int\s+main\s*\([\s\S]*?\{[\s\S]*?)(\n\})", "$1\treturn 0;\n}");
			}

			return cppCode;
		}
	}
}
