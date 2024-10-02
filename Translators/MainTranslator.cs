// Author: Pantelis Andrianakis
// Creation Date: October 1st 2024

using System.Text.RegularExpressions;

namespace Breezy.Translators
{
	class MainTranslator
	{
		public static string Process(string source)
		{
			if (source.Contains("void main()"))
			{
				// Handle void main() and replace it with int main().
				source = Regex.Replace(source, @"void\s+main\s*\(\)", "int main()");

				// Apply argument handling and return logic inside main().
				source = InsertArgsAndReturn(source);
			}
			else if (source.Contains("void main(int argc, std::string argv[])"))
			{
				// Handle void main(int argc, std::string argv[]) and replace it with int main(int argc, char* argv[]).
				source = Regex.Replace(source, @"void\s+main\s*\(int\s+argc\s*,\s*std::string\s+argv\[\]\)", "int main(int argc, char* argv[])");

				// Apply argument handling and return logic inside main().
				source = InsertArgsAndReturn(source);
			}

			return source;
		}

		private static string InsertArgsAndReturn(string source)
		{
			// Insert the std::string args logic inside main() with arguments, if applicable.
			source = Regex.Replace(source, @"int\s+main\s*\(int\s+argc,\s*char\*\s*argv\[\]\s*\)\s*\{", match =>
			{
				return match.Value + "\n\tstd::string args[argc];\n\tfor(int i = 0; i < argc; ++i) args[i] = std::string(argv[i]);";
			});

			// Final regex to ensure return 0; is added just before the last closing brace of main().
			// This uses a negative lookahead to ensure it matches only the last closing brace.
			source = Regex.Replace(source, @"(int\s+main\s*\([\s\S]*?\{[\s\S]*?)(\})\s*(?![\s\S]*\})", "$1\treturn 0;\n}");

			return source;
		}
	}
}
