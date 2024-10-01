// Author: Pantelis Andrianakis
// Creation Date: October 1st 2024

using System.Collections.Generic;
using System.Text;
using System.Text.RegularExpressions;

namespace Breezy.Translators
{
	public class FileTranslator : BaseLibrary
	{
		public static string Translate(string source)
		{
			// Define regex patterns for File.Read, File.ReadLines, File.WriteLine, File.Write, File.Append, File.Delete, and File.Exists.
			string readPattern = @"File\.Read\(([^;]+)\);";
			string readLinesPattern = @"File\.ReadLines\(([^;]+)\);";
			string writeLinePattern = @"File\.WriteLine\(([^;]+),\s*([^;]+)\);";
			string writePattern = @"File\.Write\(([^;]+),\s*([^;]+)\);";
			string appendPattern = @"File\.Append\(([^;]+),\s*([^;]+)\);";
			string deletePattern = @"File\.Delete\(([^;]+)\);";
			string existsPattern = @"File\.Exists\(([^;]+)\);";

			bool foundRead = false;
			bool foundReadLines = false;
			bool foundWriteLine = false;
			bool foundWrite = false;
			bool foundAppend = false;
			bool foundDelete = false;
			bool foundExists = false;

			// Replace File.Read and track if found.
			source = Regex.Replace(source, readPattern, match =>
			{
				foundRead = true;
				string fileName = match.Groups[1].Value;
				return $"FileRead({fileName});";
			});

			// Replace File.ReadLines and track if found.
			source = Regex.Replace(source, readLinesPattern, match =>
			{
				foundReadLines = true;
				string fileName = match.Groups[1].Value;
				return $"FileReadLines({fileName});";
			});

			// Replace File.WriteLine and track if found.
			source = Regex.Replace(source, writeLinePattern, match =>
			{
				foundWriteLine = true;
				string fileName = match.Groups[1].Value;
				string text = match.Groups[2].Value;
				return $"FileWriteLine({fileName}, {text});";
			});

			// Replace File.Write and track if found.
			source = Regex.Replace(source, writePattern, match =>
			{
				foundWrite = true;
				string fileName = match.Groups[1].Value;
				string text = match.Groups[2].Value;
				return $"FileWrite({fileName}, {text});";
			});

			// Replace File.Append and track if found.
			source = Regex.Replace(source, appendPattern, match =>
			{
				foundAppend = true;
				string fileName = match.Groups[1].Value;
				string text = match.Groups[2].Value;
				return $"FileAppend({fileName}, {text});";
			});

			// Replace File.Delete and track if found.
			source = Regex.Replace(source, deletePattern, match =>
			{
				foundDelete = true;
				string fileName = match.Groups[1].Value;
				return $"FileDelete({fileName});";
			});

			// Replace File.Exists and track if found.
			source = Regex.Replace(source, existsPattern, match =>
			{
				foundExists = true;
				string fileName = match.Groups[1].Value;
				return $"FileExists({fileName});";
			});

			// Add the necessary C++ methods if they are used.
			StringBuilder header = new StringBuilder();

			// Append FileRead method if it was found.
			if (foundRead)
			{
				header.AppendLine("std::string FileRead(const std::string& fileName)");
				header.AppendLine("{");
				header.AppendLine("\tstd::ifstream file(fileName);");
				header.AppendLine("\tstd::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());");
				header.AppendLine("\treturn content;");
				header.AppendLine("}\n");
			}

			// Append FileReadLines method if it was found.
			if (foundReadLines)
			{
				header.AppendLine("std::vector<std::string> FileReadLines(const std::string& fileName)");
				header.AppendLine("{");
				header.AppendLine("\tstd::ifstream file(fileName);");
				header.AppendLine("\tstd::vector<std::string> lines;");
				header.AppendLine("\tstd::string line;");
				header.AppendLine("\twhile (std::getline(file, line)) {");
				header.AppendLine("\t\tlines.push_back(line);");
				header.AppendLine("\t}");
				header.AppendLine("\treturn lines;");
				header.AppendLine("}\n");
			}

			// Append FileWriteLine method if it was found.
			if (foundWriteLine)
			{
				header.AppendLine("void FileWriteLine(const std::string& fileName, const std::string& text)");
				header.AppendLine("{");
				header.AppendLine("\tstd::ofstream file(fileName, std::ios_base::app);");
				header.AppendLine("\tfile << text << std::endl;");
				header.AppendLine("}\n");
			}

			// Append FileWrite method if it was found.
			if (foundWrite)
			{
				header.AppendLine("void FileWrite(const std::string& fileName, const std::string& text)");
				header.AppendLine("{");
				header.AppendLine("\tstd::ofstream file(fileName);");
				header.AppendLine("\tfile << text;");
				header.AppendLine("}\n");
			}

			// Append FileAppend method if it was found.
			if (foundAppend)
			{
				header.AppendLine("void FileAppend(const std::string& fileName, const std::string& text)");
				header.AppendLine("{");
				header.AppendLine("\tstd::ofstream file(fileName, std::ios_base::app);");
				header.AppendLine("\tfile << text;");
				header.AppendLine("}\n");
			}

			// Append FileDelete method if it was found.
			if (foundDelete)
			{
				if (!source.Contains("#include <cstdio>"))
				{
					bool addEmptyLine = !source.StartsWith("#");
					if (addEmptyLine)
					{
						source = "#include <cstdio>\n\n" + source;
					}
					else
					{
						source = "#include <cstdio>\n" + source;
					}
				}
				header.AppendLine("void FileDelete(const std::string& fileName)");
				header.AppendLine("{");
				header.AppendLine("\tstd::remove(fileName.c_str());");
				header.AppendLine("}\n");
			}

			// Append FileExists method if it was found.
			if (foundExists)
			{
				if (!source.Contains("#include <fstream>"))
				{
					bool addEmptyLine = !source.StartsWith("#");
					if (addEmptyLine)
					{
						source = "#include <fstream>\n\n" + source;
					}
					else
					{
						source = "#include <fstream>\n" + source;
					}
				}
				header.AppendLine("bool FileExists(const std::string& fileName)");
				header.AppendLine("{");
				header.AppendLine("\tstd::ifstream file(fileName);");
				header.AppendLine("\treturn file.good();");
				header.AppendLine("}\n");
			}

			source = AddHeader(source, header.ToString());

			return source;
		}
	}
}
