// Author: Pantelis Andrianakis
// Creation Date: October 1st 2024

using System.Collections.Generic;
using System.Text;
using System.Text.RegularExpressions;

namespace Breezy.Translators
{
	public class ConsoleTranslator : BaseLibrary
	{
		public static string Process(string source)
		{
			// Define the regex patterns to find Console.Write, Console.WriteLine, Console.Read, and Console.ReadLine.
			string writePattern = @"Console\.write\(([^;]+)\);";
			string writeLinePattern = @"Console\.writeLine\(([^;]+)\);";
			string readPattern = @"Console\.read\(\);";
			string readLinePattern = @"Console\.readLine\(\);";

			bool foundWrite = false;
			bool foundWriteLine = false;
			bool foundRead = false;
			bool foundReadLine = false;

			// First, replace Console.WriteLine with ConsoleWriteLine and track if found.
			source = Regex.Replace(source, writeLinePattern, match =>
			{
				foundWriteLine = true;
				string content = match.Groups[1].Value;
				// Handle string concatenation by calling the HandleConcatenation function.
				return $"consoleWriteLine({HandleConcatenation(content)});";
			});

			// Then, replace Console.Write with ConsoleWrite and track if found.
			source = Regex.Replace(source, writePattern, match =>
			{
				foundWrite = true;
				string content = match.Groups[1].Value;
				// Handle string concatenation by calling the HandleConcatenation function.
				return $"consoleWrite({HandleConcatenation(content)});";
			});

			// Replace Console.ReadLine with ConsoleReadLine and track if found.
			source = Regex.Replace(source, readLinePattern, match =>
			{
				foundReadLine = true;
				return "consoleReadLine();";
			});

			// Replace Console.Read with ConsoleRead and track if found.
			source = Regex.Replace(source, readPattern, match =>
			{
				foundRead = true;
				return "consoleRead();";
			});

			// Add the necessary C++ methods if they are used.
			// We start with a blank C++ file.
			StringBuilder header = new StringBuilder();

			// Check if we need to add <iostream> for input/output.
			if (foundWrite || foundWriteLine || foundRead || foundReadLine)
			{
				if (!source.Contains("#include <iostream>"))
				{
					bool addEmptyLine = !source.StartsWith("#");
					if (addEmptyLine)
					{
						source = "#include <iostream>\n\n" + source;
					}
					else
					{
						source = "#include <iostream>\n" + source;
					}
				}
			}

			// Append ConsoleWriteLine method if it was found.
			if (foundWriteLine)
			{
				header.AppendLine("void consoleWriteLine(const std::string& output)");
				header.AppendLine("{");
				header.AppendLine("\tstd::cout << output << std::endl;");
				header.AppendLine("}\n");
			}

			// Append ConsoleWrite method if it was found.
			if (foundWrite)
			{
				header.AppendLine("void consoleWrite(const std::string& output)");
				header.AppendLine("{");
				header.AppendLine("\tstd::cout << output;");
				header.AppendLine("}\n");
			}

			// Append ConsoleReadLine method if it was found.
			if (foundReadLine)
			{
				header.AppendLine("std::string consoleReadLine()");
				header.AppendLine("{");
				header.AppendLine("\tstd::string input;");
				header.AppendLine("\tstd::cin >> input;");
				header.AppendLine("\treturn input;");
				header.AppendLine("}\n");
			}

			// Append ConsoleRead method if it was found.
			if (foundRead)
			{
				header.AppendLine("std::string consoleRead()");
				header.AppendLine("{");
				header.AppendLine("\tstd::string key;");
				header.AppendLine("\tstd::getline(std::cin, key);");
				header.AppendLine("\treturn key;");
				header.AppendLine("}\n");
			}

			// Parent class manages adding the header.
			source = AddHeader(source, header.ToString());

			return source;
		}

		private static string HandleConcatenation(string content)
		{
			// Protect string literals from being wrapped in std::to_string.
			string stringLiteralPattern = "\"[^\"]*\"";
			List<string> literals = new List<string>();
			MatchCollection matches = Regex.Matches(content, stringLiteralPattern);

			// Temporarily replace string literals with placeholders to avoid modifying them.
			for (int i = 0; i < matches.Count; i++)
			{
				literals.Add(matches[i].Value);
				content = content.Replace(matches[i].Value, $"__STR_LITERAL_{i}__");
			}

			// Handle numeric expressions like (i + 1) and wrap them in std::to_string if needed
			// but not inside a parenthesis that includes both numeric and alphanumeric content.
			string expressionPattern = @"(?<![\w\+\-])(\d+|i\s*\+\s*1)(?![\w])";
			content = Regex.Replace(content, expressionPattern, match =>
			{
				string matchedValue = match.Groups[1].Value;
				return $"std::to_string({matchedValue})";
			});

			// Avoid replacing '+' with '<<' inside parentheses containing both alphanumeric and numeric values.
			string parenthesesPattern = @"\([\w\s\""\+\-]*\)";
			MatchCollection parenthesesMatches = Regex.Matches(content, parenthesesPattern);
			List<string> parenthesizedExpressions = new List<string>();

			// Temporarily replace such expressions with placeholders.
			for (int i = 0; i < parenthesesMatches.Count; i++)
			{
				parenthesizedExpressions.Add(parenthesesMatches[i].Value);
				content = content.Replace(parenthesesMatches[i].Value, $"__PAREN_EXPR_{i}__");
			}

			// Now safely replace '+' with '<<' for the rest of the content.
			content = content.Replace("+", "<<");

			// Restore the parenthesized expressions and string literals.
			for (int i = 0; i < parenthesizedExpressions.Count; i++)
			{
				content = content.Replace($"__PAREN_EXPR_{i}__", parenthesizedExpressions[i]);
			}

			for (int i = 0; i < literals.Count; i++)
			{
				content = content.Replace($"__STR_LITERAL_{i}__", literals[i]);
			}

			return content;
		}
	}
}
