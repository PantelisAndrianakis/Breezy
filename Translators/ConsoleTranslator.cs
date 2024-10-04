// Author: Pantelis Andrianakis
// Creation Date: October 1st 2024

using System.Collections.Generic;
using System.Text;
using System.Text.RegularExpressions;

namespace Breezy.Translators
{
	class ConsoleTranslator : MethodLibrary
	{
		public static string Process(string source)
		{
			bool foundWrite = false;
			bool foundWriteLine = false;
			bool foundRead = false;
			bool foundReadLine = false;

			// Support for random method names to avoid conflicts.
			string consoleWriteLineSuffix = "";
			string consoleWriteSuffix = "";
			string consoleReadLineSuffix = "";
			string consoleReadSuffix = "";

			// Support for empty parameters.
			source = source.Replace("Console.WriteLine()", "Console.WriteLine(\"\")");
			source = source.Replace("Console.Write()", "Console.Write(\"\")");

			// Define the regex patterns to find Console.write, Console.writeLine, Console.read, and Console.readLine.
			string writePattern = @"Console\.(?i)write\(([^;]+)\)";
			string writeLinePattern = @"Console\.(?i)writeLine\(([^;]+)\)";
			string readPattern = @"Console\.(?i)read\(\)";
			string readLinePattern = @"Console\.(?i)readLine\(\)";

			// First, replace Console.writeLine with consoleWriteLine and track if found.
			source = Regex.Replace(source, writeLinePattern, match =>
			{
				foundWriteLine = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("consoleWriteLine("))
				{
					consoleWriteLineSuffix = GetRandomMethodIdentifier();
				}
				// Handle string concatenation by calling the HandleConcatenation function.
				string content = match.Groups[1].Value;
				return $"consoleWriteLine{consoleWriteLineSuffix}({HandleConcatenation(content)})";
			});

			// Then, replace Console.write with consoleWrite and track if found.
			source = Regex.Replace(source, writePattern, match =>
			{
				foundWrite = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("consoleWrite("))
				{
					consoleWriteSuffix = GetRandomMethodIdentifier();
				}
				// Handle string concatenation by calling the HandleConcatenation function.
				string content = match.Groups[1].Value;
				return $"consoleWrite{consoleWriteSuffix}({HandleConcatenation(content)})";
			});

			// Replace Console.readLine with consoleReadLine and track if found.
			source = Regex.Replace(source, readLinePattern, match =>
			{
				foundReadLine = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("consoleReadLine()"))
				{
					consoleReadLineSuffix = GetRandomMethodIdentifier();
				}
				return $"consoleReadLine{consoleReadLineSuffix}()";
			});

			// Replace Console.read with consoleRead and track if found.
			source = Regex.Replace(source, readPattern, match =>
			{
				foundRead = true;
				if (Config.RANDOM_METHOD_PREFIX || source.Contains("consoleRead()"))
				{
					consoleReadSuffix = GetRandomMethodIdentifier();
				}
				return $"consoleRead{consoleReadSuffix}()";
			});

			// Add the necessary C++ methods if they are used.
			StringBuilder methods = new StringBuilder();

			// Check if we need to add the <iostream> import.
			if (foundWrite || foundWriteLine || foundRead || foundReadLine)
			{
				source = AddInclude(source, "iostream");
				source = AddInclude(source, "string");
			}

			// Append consoleWriteLine method if it was found.
			if (foundWriteLine)
			{
				methods.AppendLine($"void consoleWriteLine{consoleWriteLineSuffix}(const std::string& output)");
				methods.AppendLine("{");
				methods.AppendLine("\tstd::cout << output << std::endl;");
				methods.AppendLine("}\n");
			}

			// Append consoleWrite method if it was found.
			if (foundWrite)
			{
				methods.AppendLine($"void consoleWrite{consoleWriteSuffix}(const std::string& output)");
				methods.AppendLine("{");
				methods.AppendLine("\tstd::cout << output;");
				methods.AppendLine("}\n");
			}

			// Append consoleReadLine method if it was found.
			if (foundReadLine)
			{
				methods.AppendLine($"std::string consoleReadLine{consoleReadLineSuffix}()");
				methods.AppendLine("{");
				methods.AppendLine("\tstd::string input;");
				methods.AppendLine("\tstd::getline(std::cin, key);");
				methods.AppendLine("\treturn input;");
				methods.AppendLine("}\n");
			}

			// Append consoleRead method if it was found.
			if (foundRead)
			{
				methods.AppendLine($"std::string consoleRead{consoleReadSuffix}()");
				methods.AppendLine("{");
				methods.AppendLine("\tstd::string key;");
				methods.AppendLine("\tstd::cin >> input;");
				methods.AppendLine("\treturn key;");
				methods.AppendLine("}\n");
			}

			// Parent class manages adding the additional methods.
			source = AddMethods(source, methods.ToString());

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

			// Handle numeric expressions like (i + 1) and wrap them in std::to_string if needed.
			string expressionPattern = @"(?<![\w\+\-])(\d+|i\s*\+\s*\d+)(?![\w])";
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
			// content = content.Replace("+", "<<");

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
