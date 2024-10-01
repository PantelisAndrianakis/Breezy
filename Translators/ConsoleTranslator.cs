// Author: Pantelis Andrianakis
// Creation Date: October 1st 2024

using System.Collections.Generic;
using System.Text;
using System.Text.RegularExpressions;

namespace Breezy.Translators
{
	public class ConsoleTranslator : BaseLibrary
	{
		public static string Translate(string sourceCode)
		{
			// Define the regex patterns to find Console.Write, Console.WriteLine, Console.Read, and Console.ReadLine.
			string writePattern = @"Console\.Write\(([^;]+)\);";
			string writeLinePattern = @"Console\.WriteLine\(([^;]+)\);";
			string readPattern = @"Console\.Read\(\);";
			string readLinePattern = @"Console\.ReadLine\(\);";

			bool foundWrite = false;
			bool foundWriteLine = false;
			bool foundRead = false;
			bool foundReadLine = false;

			// First, replace Console.WriteLine with ConsoleWriteLine and track if found.
			string cppCode = Regex.Replace(sourceCode, writeLinePattern, match =>
			{
				foundWriteLine = true;
				string content = match.Groups[1].Value;
				// Handle string concatenation by calling the HandleConcatenation function.
				return $"ConsoleWriteLine({HandleConcatenation(content)});";
			});

			// Then, replace Console.Write with ConsoleWrite and track if found.
			cppCode = Regex.Replace(cppCode, writePattern, match =>
			{
				foundWrite = true;
				string content = match.Groups[1].Value;
				// Handle string concatenation by calling the HandleConcatenation function.
				return $"ConsoleWrite({HandleConcatenation(content)});";
			});

			// Replace Console.ReadLine with ConsoleReadLine and track if found.
			cppCode = Regex.Replace(cppCode, readLinePattern, match =>
			{
				foundReadLine = true;
				return "ConsoleReadLine();";
			});

			// Replace Console.Read with ConsoleRead and track if found.
			cppCode = Regex.Replace(cppCode, readPattern, match =>
			{
				foundRead = true;
				return "ConsoleRead();";
			});

			// Add the necessary C++ methods if they are used.
			// We start with a blank C++ file.
			StringBuilder cppHeader = new StringBuilder();

			// Append ConsoleWriteLine method if it was found.
			if (foundWriteLine)
			{
				cppHeader.AppendLine("void ConsoleWriteLine(const std::string& output)");
				cppHeader.AppendLine("{");
				cppHeader.AppendLine("\tstd::cout << output << std::endl;");
				cppHeader.AppendLine("}\n");
			}

			// Append ConsoleWrite method if it was found.
			if (foundWrite)
			{
				cppHeader.AppendLine("void ConsoleWrite(const std::string& output)");
				cppHeader.AppendLine("{");
				cppHeader.AppendLine("\tstd::cout << output;");
				cppHeader.AppendLine("}\n");
			}

			// Append ConsoleReadLine method if it was found.
			if (foundReadLine)
			{
				cppHeader.AppendLine("std::string ConsoleReadLine()");
				cppHeader.AppendLine("{");
				cppHeader.AppendLine("\tstd::string input;");
				cppHeader.AppendLine("\tstd::cin >> input;");
				cppHeader.AppendLine("\treturn input;");
				cppHeader.AppendLine("}\n");
			}

			// Append ConsoleRead method if it was found.
			if (foundRead)
			{
				cppHeader.AppendLine("std::string ConsoleRead()");
				cppHeader.AppendLine("{");
				cppHeader.AppendLine("\tstd::string key;");
				cppHeader.AppendLine("\tstd::getline(std::cin, key);");
				cppHeader.AppendLine("\treturn key;");
				cppHeader.AppendLine("}\n");
			}

			// Parent class manages adding the header.
			cppCode = AddHeader(cppCode, cppHeader.ToString());

			return cppCode;
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
