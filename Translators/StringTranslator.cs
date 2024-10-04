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

			// Find and replace (XXXX).toString() with std::to_string(XXXX), ensuring correct parentheses matching.
			while (source.Contains(").toString()") || source.Contains(").ToString()"))
			{
				foundString = true;

				// Look for `.toString()` first, then `.ToString()`.
				int index = source.IndexOf(").toString()");
				if (index == -1)
				{
					index = source.IndexOf(").ToString()");
				}

				// Iterate backwards from the position of ").toString()" to find "(".
				if (index != -1)
				{
					int parenCloseCount = 1;
					int parenOpenCount = 0;
					for (int i = index - 1; i >= 0; i--)
					{
						if (source[i] == ')')
						{
							parenCloseCount++;
						}
						else if (source[i] == '(')
						{
							parenOpenCount++;
						}

						if (parenOpenCount == parenCloseCount)
						{
							source = source.Remove(index, 11);
							source = source.Insert(i, "std::to_string");
							break;
						}
					}
				}
			}

			// Use regex to replace "string" as a type declaration with "std::string".
			string stringDeclarationPattern = @"\bstring\b\s+([a-zA-Z_][a-zA-Z0-9_]*(\s*(?:[=,*&]\s*)?[a-zA-Z_][a-zA-Z0-9_]*|\s*\[\d*\])*)\b";
			source = Regex.Replace(source, stringDeclarationPattern, match =>
			{
				foundString = true;
				return $"std::string {match.Groups[1].Value}";
			});

			// Support for random method names to avoid conflicts.
			string stringSplitSuffix = "";

			// Replace String.split with stringSplit and track if found.
			string splitPattern = @"String\.(?i)split\(\s*([^\s,]+)\s*,\s*([^\s\)]+)\s*\)";
			source = Regex.Replace(source, splitPattern, match =>
			{
				foundSplit = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("stringSplit("))
				{
					stringSplitSuffix = GetRandomMethodIdentifier();
				}
				string text = match.Groups[1].Value;
				string delimiter = match.Groups[2].Value;
				return $"stringSplit({text}, {delimiter})";
			});

			// Add necessary C++ methods if they are used.
			StringBuilder methods = new StringBuilder();

			// Add the split method if found.
			if (foundSplit)
			{
				// Add necessary #include statements.
				source = AddInclude(source, "sstream");
				source = AddInclude(source, "vector");

				methods.AppendLine($"std::vector<std::string> stringSplit{stringSplitSuffix}(const std::string& str, const std::string& delimiter)");
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
