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

			// Handle String.split(text, delimiter) and translate it to C++.
			string splitPattern = @"String\.split\(\s*([^\s,]+)\s*,\s*([^\s\)]+)\s*\)";
			source = Regex.Replace(source, splitPattern, match =>
			{
				foundSplit = true;
				string text = match.Groups[1].Value;
				string delimiter = match.Groups[2].Value;
				return $"split({text}, {delimiter})";
			});

			// Find and replace (XXXX).toString() with std::to_string(XXXX), ensuring correct parentheses matching.
			string toStringPattern = @"\(([^()]+)\)\.toString\(\)";
			source = Regex.Replace(source, toStringPattern, match =>
			{
				return "std::to_string(" + match.Groups[1].Value + ")";
			});

			// Add necessary C++ methods if they are used.
			StringBuilder header = new StringBuilder();

			// Add the split method if found.
			if (foundSplit)
			{
				if (!source.Contains("#include <sstream>"))
				{
					bool addEmptyLine = !source.StartsWith("#");
					if (addEmptyLine)
					{
						source = "#include <sstream>\n\n" + source;
					}
					else
					{
						source = "#include <sstream>\n" + source;
					}
				}

				if (!source.Contains("#include <vector>"))
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

				header.AppendLine("std::vector<std::string> split(const std::string& str, const std::string& delimiter)");
				header.AppendLine("{");
				header.AppendLine("\tstd::vector<std::string> tokens;");
				header.AppendLine("\tsize_t start = 0;");
				header.AppendLine("\tsize_t end = str.find(delimiter);");
				header.AppendLine("\twhile (end != std::string::npos)");
				header.AppendLine("\t{");
				header.AppendLine("\t\ttokens.push_back(str.substr(start, end - start));");
				header.AppendLine("\t\tstart = end + delimiter.length();");
				header.AppendLine("\t\tend = str.find(delimiter, start);");
				header.AppendLine("\t}");
				header.AppendLine("\ttokens.push_back(str.substr(start));");
				header.AppendLine("\treturn tokens;");
				header.AppendLine("}\n");
			}

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

			// Parent class manages adding the header.
			source = AddHeader(source, header.ToString());

			return source;
		}
	}
}
