// Author: Pantelis Andrianakis
// Creation Date: October 2nd 2024

using System.Text;
using System.Text.RegularExpressions;

namespace Breezy.Translators
{
	class RegexTranslator : MethodLibrary
	{
		public static string Process(string source)
		{
			bool foundMatch = false;
			bool foundMatches = false;
			bool foundReplace = false;

			// Support for random method names to avoid conflicts.
			string regexMatchSuffix = "";
			string regexMatchesSuffix = "";
			string regexReplaceSuffix = "";

			// Define the regex patterns to find Regex.regexMatch, Regex.regexMatches, and Regex.regexReplace.
			string matchPattern = @"Regex\.(?i)match\(([^;]+),\s*([^;]+)\)";
			string matchesPattern = @"Regex\.(?i)matches\(([^;]+),\s*([^;]+)\)";
			string replacePattern = @"Regex\.(?i)replace\(([^;]+),\s*([^;]+),\s*([^;]+)\)";

			// Replace Regex.match with regexMatch and track if found.
			source = Regex.Replace(source, matchPattern, match =>
			{
				foundMatch = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("regexMatch("))
				{
					regexMatchSuffix = GetRandomMethodIdentifier();
				}
				string text = match.Groups[1].Value;
				string pattern = match.Groups[2].Value;
				return $"regexMatch{regexMatchSuffix}({text}, {pattern})";
			});

			// Replace Regex.matches with regexMatches and track if found.
			source = Regex.Replace(source, matchesPattern, match =>
			{
				foundMatches = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("regexMatches("))
				{
					regexMatchesSuffix = GetRandomMethodIdentifier();
				}
				string text = match.Groups[1].Value;
				string pattern = match.Groups[2].Value;
				return $"regexMatches{regexMatchesSuffix}({text}, {pattern})";
			});

			// Replace Regex.replace with regexReplace and track if found.
			source = Regex.Replace(source, replacePattern, match =>
			{
				foundReplace = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("regexReplace("))
				{
					regexReplaceSuffix = GetRandomMethodIdentifier();
				}
				string text = match.Groups[1].Value;
				string pattern = match.Groups[2].Value;
				string replacement = match.Groups[3].Value;
				return $"regexReplace{regexReplaceSuffix}({text}, {pattern}, {replacement})";
			});

			// Add the necessary C++ methods if they are used.
			StringBuilder methods = new StringBuilder();

			// Check if we need to add the <regex> import.
			if (foundMatch || foundMatches || foundReplace)
			{
				source = AddInclude(source, "regex");
				source = AddInclude(source, "string");
				if (foundMatches)
				{
					source = AddInclude(source, "vector");
				}
			}

			// Append regexMatch method if it was found.
			if (foundMatch)
			{
				methods.AppendLine($"std::string regexMatch{regexMatchSuffix}(const std::string& text, const std::string& pattern)");
				methods.AppendLine("{");
				methods.AppendLine("\tstd::regex regexPattern(pattern);");
				methods.AppendLine("\tstd::smatch match;");
				methods.AppendLine("\tif (std::regex_search(text, match, regexPattern))");
				methods.AppendLine("\t{");
				methods.AppendLine("\t\treturn match.str(0); // Return the first match.");
				methods.AppendLine("\t}");
				methods.AppendLine("\treturn \"\"; // Return empty string if no match is found.");
				methods.AppendLine("}\n");
			}

			// Append regexMatches method if it was found.
			if (foundMatches)
			{
				methods.AppendLine($"std::vector<std::string> regexMatches{regexMatchesSuffix}(const std::string& text, const std::string& pattern)");
				methods.AppendLine("{");
				methods.AppendLine("\tstd::regex regexPattern(pattern);");
				methods.AppendLine("\tstd::sregex_iterator begin(text.begin(), text.end(), regexPattern);");
				methods.AppendLine("\tstd::sregex_iterator end;");
				methods.AppendLine("\tstd::vector<std::string> matches;");
				methods.AppendLine("\tfor (std::sregex_iterator i = begin; i != end; ++i)");
				methods.AppendLine("\t{");
				methods.AppendLine("\t\tmatches.push_back((*i).str());");
				methods.AppendLine("\t}");
				methods.AppendLine("\treturn matches;");
				methods.AppendLine("}\n");
			}

			// Append regexReplace method if it was found.
			if (foundReplace)
			{
				methods.AppendLine($"std::string regexReplace{regexReplaceSuffix}(const std::string& text, const std::string& pattern, const std::string& replacement)");
				methods.AppendLine("{");
				methods.AppendLine("\tstd::regex regexPattern(pattern);");
				methods.AppendLine("\treturn std::regex_replace(text, regexPattern, replacement);");
				methods.AppendLine("}\n");
			}

			// Parent class manages adding the additional methods.
			source = AddMethods(source, methods.ToString());

			return source;
		}
	}
}
