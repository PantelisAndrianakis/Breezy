// Author: Pantelis Andrianakis
// Creation Date: October 1st 2024

using System;
using System.Text.RegularExpressions;

namespace Breezy.Translators
{
	public class StringTranslator
	{
		public static string Translate(string source)
		{
			bool foundString = false;
			bool foundVector = false;
			bool foundMap = false;

			// Use regex to replace "string" as a type declaration with "std::string".
			string stringDeclarationPattern = @"\bstring\b\s+([a-zA-Z_][a-zA-Z0-9_]*(\s*(?:[=,*&]\s*)?[a-zA-Z_][a-zA-Z0-9_]*|\s*\[\d*\])*)\b";
			source = Regex.Replace(source, stringDeclarationPattern, match =>
			{
				foundString = true;
				return $"std::string {match.Groups[1].Value}";
			});

			// Replace occurrences of `std::vector<string>` with `std::vector<std::string>`.
			string vectorStringPattern = @"std::vector<string>";
			source = Regex.Replace(source, vectorStringPattern, "std::vector<std::string>");
			foundVector = true;

			// Replace occurrences of `std::unordered_map<string, T>` and `std::unordered_map<T, string>` with `std::unordered_map<std::string, T>`.
			string mapStringPattern1 = @"std::unordered_map<string,\s*([^\>]+)>";
			string mapStringPattern2 = @"std::unordered_map<([^\>]+),\s*string>";
			string mapStringPattern3 = @"std::unordered_map<string,\s*string>";

			// Replace `std::unordered_map<string, T>` with `std::unordered_map<std::string, T>`.
			source = Regex.Replace(source, mapStringPattern1, match =>
			{
				foundMap = true;
				string valueType = match.Groups[1].Value; // Capture the value type.
				return $"std::unordered_map<std::string, {valueType}>";
			});

			// Replace `std::unordered_map<T, string>` with `std::unordered_map<T, std::string>`.
			source = Regex.Replace(source, mapStringPattern2, match =>
			{
				foundMap = true;
				string keyType = match.Groups[1].Value; // Capture the key type.
				return $"std::unordered_map<{keyType}, std::string>";
			});

			// Replace `std::unordered_map<string, string>` with `std::unordered_map<std::string, std::string>`.
			source = Regex.Replace(source, mapStringPattern3, match =>
			{
				foundMap = true; // Set foundMap to true only when a match is found.
				return "std::unordered_map<std::string, std::string>";
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
			if (foundVector && !source.Contains("#include <vector>"))
			{
				bool addEmptyLine = !source.StartsWith("#");
				if (addEmptyLine)
				{
					source = "#include <vector>\n\n" + source;
				}
				else
				{
					source = "#include <vector>\n" + source;
				}
			}
			if (foundMap && !source.Contains("#include <unordered_map>"))
			{
				bool addEmptyLine = !source.StartsWith("#");
				if (addEmptyLine)
				{
					source = "#include <unordered_map>\n\n" + source;
				}
				else
				{
					source = "#include <unordered_map>\n" + source;
				}
			}

			return source;
		}
	}
}
