// Author: Pantelis Andrianakis
// Creation Date: October 1st 2024

using System.Text.RegularExpressions;

namespace Breezy.Translators
{
	public class StringTranslator
	{
		public static string Process(string source)
		{
			bool foundString = false;

			// Use regex to replace "string" as a type declaration with "std::string".
			string stringDeclarationPattern = @"\bstring\b\s+([a-zA-Z_][a-zA-Z0-9_]*(\s*(?:[=,*&]\s*)?[a-zA-Z_][a-zA-Z0-9_]*|\s*\[\d*\])*)\b";
			source = Regex.Replace(source, stringDeclarationPattern, match =>
			{
				foundString = true;
				return $"std::string {match.Groups[1].Value}";
			});

			// Find and replace (XXXX).toString() with std::to_string(XXXX), ensuring correct parentheses matching.
			string toStringPattern = @"\(([^()]+)\)\.toString\(\)";
			source = Regex.Replace(source, toStringPattern, match =>
			{
				return "std::to_string(" + match.Groups[1].Value + ")";
			});

			// Add necessary #include statements based on found elements.
			if (foundString && !source.Contains("#include <string>"))
			{
				bool addEmptyLine = !source.StartsWith("#");
				if (addEmptyLine)
				{
					source = "#include <string>\n\n" + source;
				}
				else
				{
					source = "#include <string>\n" + source;
				}
			}

			return source;
		}
	}
}
