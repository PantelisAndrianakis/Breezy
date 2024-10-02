// Author: Pantelis Andrianakis
// Creation Date: October 1st 2024

using System.Text;
using System.Text.RegularExpressions;

namespace Breezy.Translators
{
	class FileTranslator : MethodLibrary
	{
		public static string Process(string source)
		{
			// Support for random method names to avoid conflicts.
			string random = GetRandomMethodIdentifier();

			// Define regex patterns for File.Read, File.ReadLines, File.WriteLine, File.Write, File.Append, File.Delete, and File.Exists.
			string readPattern = @"File\.(?i)read\(([^;]+)\);";
			string readLinesPattern = @"File\.(?i)readLines\(([^;]+)\);";
			string writeLinePattern = @"File\.(?i)writeLine\(([^;]+),\s*([^;]+)\);";
			string writePattern = @"File\.(?i)write\(([^;]+),\s*([^;]+)\);";
			string appendPattern = @"File\.(?i)append\(([^;]+),\s*([^;]+)\);";
			string deletePattern = @"File\.(?i)delete\(([^;]+)\);";
			string existsPattern = @"File\.(?i)exists\(([^;]+)\);";

			bool foundRead = false;
			bool foundReadLines = false;
			bool foundWriteLine = false;
			bool foundWrite = false;
			bool foundAppend = false;
			bool foundDelete = false;
			bool foundExists = false;

			// Replace File.read and track if found.
			source = Regex.Replace(source, readPattern, match =>
			{
				foundRead = true;
				string fileName = match.Groups[1].Value;
				return $"fileRead{random}({fileName});";
			});

			// Replace File.readLines and track if found.
			source = Regex.Replace(source, readLinesPattern, match =>
			{
				foundReadLines = true;
				string fileName = match.Groups[1].Value;
				return $"fileReadLines{random}({fileName});";
			});

			// Replace File.writeLine and track if found.
			source = Regex.Replace(source, writeLinePattern, match =>
			{
				foundWriteLine = true;
				string fileName = match.Groups[1].Value;
				string text = match.Groups[2].Value;
				return $"fileWriteLine{random}({fileName}, {text});";
			});

			// Replace File.write and track if found.
			source = Regex.Replace(source, writePattern, match =>
			{
				foundWrite = true;
				string fileName = match.Groups[1].Value;
				string text = match.Groups[2].Value;
				return $"fileWrite{random}({fileName}, {text});";
			});

			// Replace File.append and track if found.
			source = Regex.Replace(source, appendPattern, match =>
			{
				foundAppend = true;
				string fileName = match.Groups[1].Value;
				string text = match.Groups[2].Value;
				return $"fileAppend{random}({fileName}, {text});";
			});

			// Replace File.delete and track if found.
			source = Regex.Replace(source, deletePattern, match =>
			{
				foundDelete = true;
				string fileName = match.Groups[1].Value;
				return $"fileDelete{random}({fileName});";
			});

			// Replace File.exists and track if found.
			source = Regex.Replace(source, existsPattern, match =>
			{
				foundExists = true;
				string fileName = match.Groups[1].Value;
				return $"fileExists{random}({fileName});";
			});

			// Add the necessary C++ methods if they are used.
			StringBuilder methods = new StringBuilder();

			// Append fileRead method if it was found.
			if (foundRead)
			{
				methods.AppendLine($"std::string fileRead{random}(const std::string& fileName)");
				methods.AppendLine("{");
				methods.AppendLine("\tstd::ifstream file(fileName);");
				methods.AppendLine("\tstd::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());");
				methods.AppendLine("\treturn content;");
				methods.AppendLine("}\n");
			}

			// Append fileReadLines method if it was found.
			if (foundReadLines)
			{
				methods.AppendLine($"std::vector<std::string> fileReadLines{random}(const std::string& fileName)");
				methods.AppendLine("{");
				methods.AppendLine("\tstd::ifstream file(fileName);");
				methods.AppendLine("\tstd::vector<std::string> lines;");
				methods.AppendLine("\tstd::string line;");
				methods.AppendLine("\twhile (std::getline(file, line))");
				methods.AppendLine("\t{");
				methods.AppendLine("\t\tlines.push_back(line);");
				methods.AppendLine("\t}");
				methods.AppendLine("\treturn lines;");
				methods.AppendLine("}\n");
			}

			// Append fileWriteLine method if it was found.
			if (foundWriteLine)
			{
				methods.AppendLine($"void fileWriteLine{random}(const std::string& fileName, const std::string& text)");
				methods.AppendLine("{");
				methods.AppendLine("\tstd::ofstream file(fileName, std::ios_base::app);");
				methods.AppendLine("\tfile << text << std::endl;");
				methods.AppendLine("}\n");
			}

			// Append fileWrite method if it was found.
			if (foundWrite)
			{
				methods.AppendLine($"void fileWrite{random}(const std::string& fileName, const std::string& text)");
				methods.AppendLine("{");
				methods.AppendLine("\tstd::ofstream file(fileName);");
				methods.AppendLine("\tfile << text;");
				methods.AppendLine("}\n");
			}

			// Append fileAppend method if it was found.
			if (foundAppend)
			{
				methods.AppendLine($"void fileAppend{random}(const std::string& fileName, const std::string& text)");
				methods.AppendLine("{");
				methods.AppendLine("\tstd::ofstream file(fileName, std::ios_base::app);");
				methods.AppendLine("\tfile << text;");
				methods.AppendLine("}\n");
			}

			// Append fileDelete method if it was found.
			if (foundDelete)
			{
				source = AddInclude(source, "cstdio");

				methods.AppendLine($"void fileDelete{random}(const std::string& fileName)");
				methods.AppendLine("{");
				methods.AppendLine("\tstd::remove(fileName.c_str());");
				methods.AppendLine("}\n");
			}

			// Append fileExists method if it was found.
			if (foundExists)
			{
				source = AddInclude(source, "fstream");

				methods.AppendLine($"bool fileExists{random}(const std::string& fileName)");
				methods.AppendLine("{");
				methods.AppendLine("\tstd::ifstream file(fileName);");
				methods.AppendLine("\treturn file.good();");
				methods.AppendLine("}\n");
			}

			// Parent class manages adding the additional methods.
			source = AddMethods(source, methods.ToString());

			return source;
		}
	}
}
