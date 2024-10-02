// Author: Pantelis Andrianakis
// Creation Date: October 1st 2024

using System.Text.RegularExpressions;
using System.Text;

namespace Breezy.Translators
{
	class StringTranslator : MethodLibrary
	{
		public static string Process(string source)
		{
			bool foundString = false;
			bool foundSplit = false;

			// Use regex to replace "string" as a type declaration with "std::string".
			string stringDeclarationPattern = @"\bstring\b\s+([a-zA-Z_][a-zA-Z0-9_]*(\s*(?:[=,*&]\s*)?[a-zA-Z_][a-zA-Z0-9_]*|\s*\[\d*\])*)\b";
			source = Regex.Replace(source, stringDeclarationPattern, match =>
			{
				foundString = true;
				return $"std::string {match.Groups[1].Value}";
			});

			// Replace String.split and track if found.
			string splitPattern = @"String\.split\(\s*([^\s,]+)\s*,\s*([^\s\)]+)\s*\)";
			source = Regex.Replace(source, splitPattern, match =>
			{
				foundSplit = true;
				string text = match.Groups[1].Value;
				string delimiter = match.Groups[2].Value;
				return $"stringSplit({text}, {delimiter})";
			});

			// Find and replace (XXXX).toString() with std::to_string(XXXX), ensuring correct parentheses matching.
			string toStringPattern = @"\(([^()]+)\)\.toString\(\)";
			source = Regex.Replace(source, toStringPattern, match =>
			{
				return "std::to_string(" + match.Groups[1].Value + ")";
			});

			// Add necessary C++ methods if they are used.
			StringBuilder methods = new StringBuilder();

			// Add the split method if found.
			if (foundSplit)
			{
				// Add necessary #include statements.
				source = AddInclude(source, "sstream");
				source = AddInclude(source, "vector");

				methods.AppendLine("std::vector<std::string> stringSplit(const std::string& str, const std::string& delimiter)");
				methods.AppendLine("{");
				methods.AppendLine("\tstd::vector<std::string> tokens;");
				methods.AppendLine("\tsize_t start = 0;");
				methods.AppendLine("\tsize_t end = str.find(delimiter);");
				methods.AppendLine("\twhile (end != std::string::npos)");
				methods.AppendLine("\t{");
				methods.AppendLine("\t\ttokens.push_back(str.substr(start, end - start));");
				methods.AppendLine("\t\tstart = end + delimiter.length();");
				methods.AppendLine("\t\tend = str.find(delimiter, start);");
				methods.AppendLine("\t}");
				methods.AppendLine("\ttokens.push_back(str.substr(start));");
				methods.AppendLine("\treturn tokens;");
				methods.AppendLine("}\n");
			}

			// Check if we need to add the <string> import.
			if (foundString)
			{
				source = AddInclude(source, "string");
			}

			// Parent class manages adding the additional methods.
			source = AddMethods(source, methods.ToString());

			return source;
		}
	}
}
