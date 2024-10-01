// Author: Pantelis Andrianakis
// Creation Date: October 1st 2024

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
				// Handle string concatenation by replacing '+' with '<<'.
				return $"ConsoleWriteLine({content.Replace("+", "<<")});";
			});

			// Then, replace Console.Write with ConsoleWrite and track if found.
			cppCode = Regex.Replace(cppCode, writePattern, match =>
			{
				foundWrite = true;
				string content = match.Groups[1].Value;
				// Handle string concatenation by replacing '+' with '<<'.
				return $"ConsoleWrite({content.Replace("+", "<<")});";
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
			AddHeader(cppCode, cppHeader.ToString());

			return cppCode;
		}
	}
}
